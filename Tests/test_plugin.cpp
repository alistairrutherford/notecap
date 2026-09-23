// Integration test: drives the real NoteCapAudioProcessor with a fake Live
// transport and a strummed-guitar sidechain, and listens on its CoreMIDI
// virtual port like Ableton would. Run via run_integration.sh.
#include "PluginProcessor.h"
#include "Synth.h"
#include "TestUtil.h"
#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <new>
#include <set>
#include <thread>

using testutil::check;

// ---- heap allocation counter: counts operator new on threads that opt in ----
namespace alloccount
{
    thread_local bool counting = false;
    std::atomic<int> count { 0 };
}

void* operator new (std::size_t n)
{
    if (alloccount::counting) alloccount::count.fetch_add (1);
    if (void* p = std::malloc (n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[] (std::size_t n) { return operator new (n); }
void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }

namespace
{
    constexpr double fs = 48000.0, bpm = 120.0;
    constexpr int block = 256;
    constexpr int64_t samplesPerBeat = (int64_t) (fs * 60.0 / bpm);   // 24000

    struct FakePlayHead : juce::AudioPlayHead
    {
        juce::AudioPlayHead::PositionInfo info;
        juce::Optional<PositionInfo> getPosition() const override { return info; }
    };

    // Listens to a CoreMIDI source by name and records note events with timestamps.
    struct MidiListener
    {
        struct Event { uint64_t hostNs; uint8_t status, data1, data2; };
        MIDIClientRef client = 0;
        MIDIPortRef port = 0;
        std::mutex lock;
        std::vector<Event> events;

        static uint64_t ticksToNs (MIDITimeStamp t)
        {
            mach_timebase_info_data_t tb; mach_timebase_info (&tb);
            return (uint64_t) ((__int128) t * tb.numer / tb.denom);
        }

        bool connect (const juce::String& sourceName)
        {
            if (MIDIClientCreateWithBlock (CFSTR ("NoteCapTest"), &client, nullptr) != noErr) return false;
            auto receive = ^(const MIDIEventList* list, void*)
            {
                const MIDIEventPacket* p = &list->packet[0];
                std::lock_guard<std::mutex> g (lock);
                for (UInt32 i = 0; i < list->numPackets; ++i)
                {
                    for (UInt32 w = 0; w < p->wordCount; ++w)
                    {
                        const UInt32 word = p->words[w];
                        if ((word >> 28) == 0x2)
                            events.push_back ({ ticksToNs (p->timeStamp), (uint8_t) ((word >> 16) & 0xff),
                                                (uint8_t) ((word >> 8) & 0xff), (uint8_t) (word & 0xff) });
                    }
                    p = MIDIEventPacketNext (p);
                }
            };
            if (MIDIInputPortCreateWithProtocol (client, CFSTR ("in"), kMIDIProtocol_1_0, &port, receive) != noErr) return false;

            for (int attempt = 0; attempt < 50; ++attempt)
            {
                for (ItemCount i = 0; i < MIDIGetNumberOfSources(); ++i)
                {
                    const MIDIEndpointRef src = MIDIGetSource (i);
                    CFStringRef name = nullptr;
                    if (MIDIObjectGetStringProperty (src, kMIDIPropertyName, &name) == noErr && name != nullptr)
                    {
                        const auto n = juce::String::fromCFString (name);
                        CFRelease (name);
                        if (n == sourceName)
                            return MIDIPortConnectSource (port, src, nullptr) == noErr;
                    }
                }
                CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false);
            }
            return false;
        }

        std::vector<Event> snapshot()
        {
            std::lock_guard<std::mutex> g (lock);
            return events;
        }

        ~MidiListener()
        {
            if (port) MIDIPortDispose (port);
            if (client) MIDIClientDispose (client);
        }
    };

    struct HostNote { int64_t sample; int note; bool on; };

    struct RunResult
    {
        std::vector<HostNote> hostNotes;
        int64_t hostStartNs = 0;
        int64_t totalSamples = 0;
    };

    void setParam (NoteCapAudioProcessor& p, const char* id, float plainValue)
    {
        auto* param = p.apvts.getParameter (id);
        param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
    }

    // Runs the processor over `sidechain` with the transport playing from ppq 0,
    // then a few stopped blocks. Returns every MIDI event it wrote to the host buffer.
    RunResult run (NoteCapAudioProcessor& proc, const std::vector<float>& sidechain, int stoppedBlocks = 8,
                   bool countAllocations = false)
    {
        FakePlayHead ph;
        proc.setPlayHead (&ph);
        proc.prepareToPlay (fs, block);

        RunResult r;
        r.hostStartNs = VirtualMidiOut::nowNs() + 100'000'000;  // a little in the future
        juce::AudioBuffer<float> buf (4, block);   // main L/R + sidechain L/R
        juce::MidiBuffer midi;
        midi.ensureSize (4096);   // the host owns this buffer; don't count its growth

        const int64_t total = (int64_t) sidechain.size() + stoppedBlocks * block;
        for (int64_t pos = 0; pos < total; pos += block)
        {
            const bool playing = pos < (int64_t) sidechain.size();
            ph.info.setIsPlaying (playing);
            ph.info.setBpm (bpm);
            ph.info.setPpqPosition ((double) pos / samplesPerBeat);
            ph.info.setHostTimeNs ((uint64_t) (r.hostStartNs + (int64_t) (pos * 1.0e9 / fs)));

            buf.clear();
            for (int i = 0; i < block && pos + i < (int64_t) sidechain.size(); ++i)
            {
                buf.setSample (2, i, sidechain[(size_t) (pos + i)]);
                buf.setSample (3, i, sidechain[(size_t) (pos + i)]);
            }
            midi.clear();
            alloccount::counting = countAllocations;
            proc.processBlock (buf, midi);
            alloccount::counting = false;
            for (const auto meta : midi)
            {
                const auto m = meta.getMessage();
                if (m.isNoteOnOrOff())
                    r.hostNotes.push_back ({ pos + meta.samplePosition, m.getNoteNumber(), m.isNoteOn() });
            }
        }
        r.totalSamples = total;
        proc.setPlayHead (nullptr);
        return r;
    }

    // Groups note-ons by sample position into chords (sorted note lists).
    std::map<int64_t, std::vector<int>> chordsBySample (const std::vector<HostNote>& notes)
    {
        std::map<int64_t, std::vector<int>> out;
        for (const auto& n : notes)
            if (n.on) out[n.sample].push_back (n.note);
        for (auto& [s, v] : out) std::sort (v.begin(), v.end());
        return out;
    }

    std::vector<int> closeVoicing (const char* name)
    {
        static const std::map<std::string, std::vector<int>> v {
            { "C", { 60, 64, 67 } }, { "Am", { 69, 72, 76 } }, { "F", { 65, 69, 72 } }, { "G", { 67, 71, 74 } } };
        return v.at (name);
    }

    // Strums a few ms either side of beats 2, 4, 6 and 8 (seconds 1..4 at 120 BPM).
    std::vector<testsynth::Strum> offGridStrums()
    {
        return { { 1.0 + 0.020, testsynth::guitarChord ("C"),  "C" },
                 { 2.0 - 0.025, testsynth::guitarChord ("Am"), "Am" },
                 { 3.0 + 0.010, testsynth::guitarChord ("F"),  "F" },
                 { 4.0 - 0.015, testsynth::guitarChord ("G"),  "G" } };
    }
}

static void testNextLineQuantize()
{
    std::cout << "== next-line quantize, host MIDI buffer ==\n";
    NoteCapAudioProcessor proc;
    proc.setRateAndBufferSizeDetails (fs, block);
    setParam (proc, "grid", 3);     // 1/4
    setParam (proc, "qmode", 1);    // next line
    setParam (proc, "virtualPort", 0);

    const auto strums = offGridStrums();
    const auto r = run (proc, testsynth::renderGuitar (strums, fs, 5.0));
    const auto chords = chordsBySample (r.hostNotes);

    bool allOnGrid = ! chords.empty();
    for (const auto& [s, v] : chords) allOnGrid &= (s % samplesPerBeat) == 0;
    check (allOnGrid, "every chord lands exactly on a beat (" + std::to_string (chords.size()) + " chords)");

    std::vector<std::vector<int>> got;
    for (const auto& [s, v] : chords) got.push_back (v);
    const std::vector<std::vector<int>> expect { closeVoicing ("C"), closeVoicing ("Am"), closeVoicing ("F"), closeVoicing ("G") };
    check (got == expect, "chord sequence C Am F G in close voicing");

    // Each strum is followed by the next beat line after detection.
    if (chords.size() == 4)
    {
        const int64_t expectBeats[] = { 3, 5, 7, 9 };   // strum ~ beat 2 + ~130 ms detection -> beat 3
        int i = 0; bool ok = true;
        for (const auto& [s, v] : chords) ok &= s == expectBeats[i++] * samplesPerBeat;
        check (ok, "next-line mode waits for the first beat after detection");
    }

    int held = 0;
    for (const auto& n : r.hostNotes) held += n.on ? 1 : -1;
    check (held == 0, "transport stop releases every held note");
}

static void testNearestWithPort()
{
    std::cout << "== nearest quantize, CoreMIDI virtual port ==\n";
    NoteCapAudioProcessor proc;
    proc.setRateAndBufferSizeDetails (fs, block);
    setParam (proc, "grid", 3);     // 1/4
    setParam (proc, "qmode", 0);    // nearest

    const auto& out = proc.getMidiOut();
    check (out.isOpen(), "virtual port opened as \"" + out.getName().toStdString() + "\"");

    MidiListener listener;
    check (listener.connect (out.getName()), "test client connects to the port");

    const auto strums = offGridStrums();
    const auto r = run (proc, testsynth::renderGuitar (strums, fs, 5.0));

    // Wait until CoreMIDI has delivered everything (timestamps may be in the future).
    const int64_t lastStampNs = r.hostStartNs + (int64_t) (r.totalSamples * 1.0e9 / fs);
    while (VirtualMidiOut::nowNs() < lastStampNs + 300'000'000)
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.05, false);

    const auto events = listener.snapshot();
    std::map<int64_t, std::vector<int>> portChords;
    int held = 0;
    for (const auto& e : events)
    {
        const bool on = (e.status & 0xf0) == 0x90 && e.data2 > 0;
        held += on ? 1 : -1;
        if (! on) continue;
        const int64_t sample = (int64_t) std::llround (((double) e.hostNs - (double) r.hostStartNs) * fs / 1.0e9);
        portChords[sample].push_back (e.data1);
    }
    check (portChords.size() == 4, "port chords grouped on 4 timestamps (got " + std::to_string (portChords.size()) + ")");

    // Nearest mode: every strum was within 30 ms of a beat, so each chord is
    // time-stamped on that beat even though detection happened ~130 ms later.
    const int64_t expectBeats[] = { 2, 4, 6, 8 };
    int i = 0; bool onBeat = portChords.size() == 4;
    int64_t worst = 0;
    for (auto& [s, v] : portChords)
    {
        if (i >= 4) break;
        const int64_t err = s - expectBeats[i++] * samplesPerBeat;
        worst = std::max (worst, std::abs (err));
        onBeat &= std::abs (err) <= 2;   // ns -> sample rounding
    }
    check (onBeat, "port timestamps sit on the played beats (worst error " + std::to_string (worst) + " samples)");

    // The host buffer can't be back-dated: those notes go out as soon as detected.
    const auto hostChords = chordsBySample (r.hostNotes);
    // The host buffer can't be back-dated: each chord goes out as soon as it is
    // detected. A revised decision only changes the notes that differ.
    std::string lateness;
    bool afterBeat = true;
    for (int k = 0; k < 4; ++k)
    {
        const int64_t beat = expectBeats[k] * samplesPerBeat;
        const auto it = hostChords.lower_bound (beat - samplesPerBeat / 2);
        const bool found = it != hostChords.end() && it->first < beat + samplesPerBeat;
        const int64_t lateMs = found ? (it->first - beat) * 1000 / (int64_t) fs : -1;
        lateness += std::to_string (lateMs) + " ";
        afterBeat &= found && lateMs >= 0 && lateMs < 250;
    }
    check (afterBeat, "host MIDI buffer sends each chord straight after detection (ms after beat: " + lateness + ")");

    // Whatever was revised along the way, the notes held on the port just before
    // each next beat must be the right chord.
    std::set<int> soundingNow;
    std::vector<std::vector<int>> heldBeforeNext;
    size_t idx = 0;
    std::vector<MidiListener::Event> sorted = events;
    std::stable_sort (sorted.begin(), sorted.end(), [] (auto& x, auto& y) { return x.hostNs < y.hostNs; });
    for (int k = 0; k < 4; ++k)
    {
        const int64_t checkAt = r.hostStartNs + (int64_t) ((expectBeats[k] + 1.9) * samplesPerBeat * 1.0e9 / fs);
        for (; idx < sorted.size() && (int64_t) sorted[idx].hostNs <= checkAt; ++idx)
        {
            const auto& e = sorted[idx];
            if ((e.status & 0xf0) == 0x90 && e.data2 > 0) soundingNow.insert (e.data1);
            else soundingNow.erase (e.data1);
        }
        heldBeforeNext.emplace_back (soundingNow.begin(), soundingNow.end());
    }
    const std::vector<std::vector<int>> expectHeld { closeVoicing ("C"), closeVoicing ("Am"), closeVoicing ("F"), closeVoicing ("G") };
    check (heldBeforeNext == expectHeld, "held notes settle on C, Am, F, G");
    check (held == 0, "port saw a note-off for every note-on");
}

static void testTestMode()
{
    std::cout << "== test-chord mode ==\n";
    NoteCapAudioProcessor proc;
    proc.setRateAndBufferSizeDetails (fs, block);
    setParam (proc, "grid", 4);     // 1/8
    setParam (proc, "testMode", 1);
    setParam (proc, "virtualPort", 0);

    const std::vector<float> silence ((size_t) (samplesPerBeat * 4), 0.0f);   // 4 beats = 8 eighths
    const auto chords = chordsBySample (run (proc, silence).hostNotes);
    bool onGrid = true;
    for (const auto& [s, v] : chords) onGrid &= s % (samplesPerBeat / 2) == 0;
    check (chords.size() == 8 && onGrid, "one test chord on every 1/8 line (" + std::to_string (chords.size()) + ")");
    if (chords.size() >= 4)
    {
        auto it = chords.begin();
        const bool cycle = it->second == closeVoicing ("C") && (++it)->second == closeVoicing ("F")
                           && (++it)->second == closeVoicing ("G") && (++it)->second == closeVoicing ("Am");
        check (cycle, "test chords cycle C F G Am");
    }
}

static void testFixedLength()
{
    std::cout << "== fixed note length ==\n";
    NoteCapAudioProcessor proc;
    proc.setRateAndBufferSizeDetails (fs, block);
    setParam (proc, "grid", 3);     // 1/4
    setParam (proc, "qmode", 1);
    setParam (proc, "length", 1);   // 1 step
    setParam (proc, "virtualPort", 0);

    const auto r = run (proc, testsynth::renderGuitar ({ { 1.0, testsynth::guitarChord ("C"), "C" } }, fs, 3.0), 0);
    int64_t on = -1, off = -1;
    for (const auto& n : r.hostNotes)
    {
        if (n.on && on < 0) on = n.sample;
        if (! n.on && off < 0) off = n.sample;
    }
    check (on >= 0 && off - on == samplesPerBeat, "note-off exactly one grid step after note-on");
}

static void testStateAndPorts()
{
    std::cout << "== state and port naming ==\n";
    auto a = std::make_unique<NoteCapAudioProcessor>();
    auto b = std::make_unique<NoteCapAudioProcessor>();
    auto c = std::make_unique<NoteCapAudioProcessor>();
    check (a->getMidiOut().getName() != b->getMidiOut().getName() && b->getMidiOut().getName() != c->getMidiOut().getName(),
           "instances get distinct ports: " + a->getMidiOut().getName().toStdString() + ", "
               + b->getMidiOut().getName().toStdString() + ", " + c->getMidiOut().getName().toStdString());

    // Save c (say "NoteCap Out 3"), then close b and c as when a Live set is closed.
    setParam (*c, "grid", 6);
    setParam (*c, "octave", 2);
    juce::MemoryBlock state;
    c->getStateInformation (state);
    const int cIndex = c->getMidiOut().getIndex();
    c.reset();
    b.reset();

    // A new instance takes the lowest free index (b's), then restoring c's
    // state must move it to c's port so the track's "MIDI From" still matches.
    auto d = std::make_unique<NoteCapAudioProcessor>();
    const int before = d->getMidiOut().getIndex();
    d->setStateInformation (state.getData(), (int) state.getSize());
    check (before != cIndex && d->getMidiOut().getIndex() == cIndex,
           "restored state reclaims its saved port (" + std::to_string (before) + " -> " + std::to_string (d->getMidiOut().getIndex()) + ")");
    check ((int) d->apvts.getRawParameterValue ("grid")->load() == 6
               && (int) d->apvts.getRawParameterValue ("octave")->load() == 2,
           "parameters survive a state round trip");

    setParam (*d, "virtualPort", 0);
    for (int i = 0; i < 20 && d->getMidiOut().isOpen(); ++i)
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.05, false);
    check (! d->getMidiOut().isOpen(), "turning the port parameter off closes the port");
}


static void testNoAllocationInProcessBlock()
{
    std::cout << "== real-time safety ==\n";
    NoteCapAudioProcessor proc;
    proc.setRateAndBufferSizeDetails (fs, block);
    setParam (proc, "grid", 3);
    setParam (proc, "length", 2);   // fixed length: exercises release scheduling too
    const auto audio = testsynth::renderGuitar ({ { 0.5, testsynth::guitarChord ("C"), "C" },
                                                  { 1.5, testsynth::guitarChord ("Am"), "Am" },
                                                  { 2.5, testsynth::guitarChord ("E7"), "E7" },
                                                  { 3.5, testsynth::guitarChord ("G"), "G" } }, fs, 4.5);
    run (proc, audio);   // warm-up
    alloccount::count = 0;
    const auto r = run (proc, audio, 8, true);
    check (! r.hostNotes.empty() && alloccount::count.load() == 0,
           "no heap allocation inside processBlock (" + std::to_string (alloccount::count.load()) + " over "
               + std::to_string (r.totalSamples / block) + " blocks, " + std::to_string (r.hostNotes.size()) + " MIDI events)");
}

static void testPortCloseReleasesNotes()
{
    std::cout << "== no stuck notes when the port closes ==\n";
    NoteCapAudioProcessor proc;
    proc.setRateAndBufferSizeDetails (fs, block);
    setParam (proc, "grid", 0);   // off: send immediately
    MidiListener listener;
    check (listener.connect (proc.getMidiOut().getName()), "listener connected");

    // Chord still held when the transport keeps running (no stop at the end).
    run (proc, testsynth::renderGuitar ({ { 0.3, testsynth::guitarChord ("D"), "D" } }, fs, 1.0), 0);
    setParam (proc, "virtualPort", 0);
    for (int i = 0; i < 40 && proc.getMidiOut().isOpen(); ++i)
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.05, false);
    // The fake host clock runs ahead of real time, so deliveries are scheduled up to ~1 s out.
    const int64_t until = VirtualMidiOut::nowNs() + 1'500'000'000LL;
    while (VirtualMidiOut::nowNs() < until)
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.05, false);

    // In delivery order, a note is stuck if the last thing the receiver heard for it was a note-on.
    std::map<int, bool> held;
    int offs = 0;
    for (const auto& e : listener.snapshot())
    {
        const bool on = (e.status & 0xf0) == 0x90 && e.data2 > 0;
        held[e.data1] = on;
        offs += on ? 0 : 1;
    }
    int stuck = 0;
    for (const auto& [n, on] : held) stuck += on ? 1 : 0;
    check (offs > 0 && stuck == 0, "closing the port leaves no stuck notes (" + std::to_string (stuck) + " stuck, "
                                       + std::to_string (offs) + " note-offs delivered)");

    // Same again with a host clock at real time (as in Live): note-ons arrive, then their releases.
    NoteCapAudioProcessor live;
    live.setRateAndBufferSizeDetails (fs, block);
    setParam (live, "grid", 0);
    MidiListener l2;
    l2.connect (live.getMidiOut().getName());
    FakePlayHead ph;
    live.setPlayHead (&ph);
    live.prepareToPlay (fs, block);
    const auto audio = testsynth::renderGuitar ({ { 0.1, testsynth::guitarChord ("A"), "A" } }, fs, 0.5);
    juce::AudioBuffer<float> buf (4, block);
    juce::MidiBuffer midi;
    for (size_t pos = 0; pos + block <= audio.size(); pos += block)
    {
        ph.info.setIsPlaying (true); ph.info.setBpm (bpm); ph.info.setPpqPosition ((double) pos / samplesPerBeat);
        ph.info.setHostTimeNs ((uint64_t) VirtualMidiOut::nowNs());
        buf.clear();
        for (int i = 0; i < block; ++i) buf.setSample (2, i, audio[pos + (size_t) i]);
        midi.clear();
        live.processBlock (buf, midi);
        std::this_thread::sleep_for (std::chrono::microseconds ((int) (block * 1.0e6 / fs)));  // real-time pacing
    }
    live.setPlayHead (nullptr);
    setParam (live, "virtualPort", 0);
    for (int i = 0; i < 40 && live.getMidiOut().isOpen(); ++i)
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.05, false);
    CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.3, false);
    int ons2 = 0; held.clear();
    for (const auto& e : l2.snapshot())
    {
        const bool on = (e.status & 0xf0) == 0x90 && e.data2 > 0;
        held[e.data1] = on; ons2 += on ? 1 : 0;
    }
    stuck = 0;
    for (const auto& [n, on] : held) stuck += on ? 1 : 0;
    check (ons2 > 0 && stuck == 0, "real-time clock: " + std::to_string (ons2) + " notes delivered, all released on close");
}

static void testRecoversFromNaN()
{
    std::cout << "== bad input ==\n";
    NoteCapAudioProcessor proc;
    proc.setRateAndBufferSizeDetails (fs, block);
    setParam (proc, "grid", 0);
    setParam (proc, "virtualPort", 0);
    auto audio = testsynth::renderGuitar ({ { 0.5, testsynth::guitarChord ("C"), "C" },
                                            { 2.0, testsynth::guitarChord ("G"), "G" } }, fs, 3.0);
    for (size_t i = (size_t) (1.2 * fs); i < (size_t) (1.3 * fs); ++i)
        audio[i] = (i % 3 == 0) ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
    const auto chords = chordsBySample (run (proc, audio).hostNotes);
    bool sawG = false;
    for (const auto& [s, v] : chords) sawG |= s > (int64_t) (1.9 * fs) && v == closeVoicing ("G");
    check (sawG, "still detects the next chord after 100 ms of NaN/Inf input");
}

static void testConcurrentInstances()
{
    std::cout << "== four instances processing concurrently ==\n";
    const char* names[] = { "C", "Am", "F", "G" };
    std::vector<std::unique_ptr<NoteCapAudioProcessor>> procs;
    std::vector<std::unique_ptr<MidiListener>> listeners;
    std::vector<std::vector<float>> audio;
    bool allConnected = true;
    for (int i = 0; i < 4; ++i)
    {
        procs.push_back (std::make_unique<NoteCapAudioProcessor>());
        procs.back()->setRateAndBufferSizeDetails (fs, block);
        setParam (*procs.back(), "grid", 3);
        setParam (*procs.back(), "channel", (float) (i + 1));   // tags every message with its instance
        listeners.push_back (std::make_unique<MidiListener>());
        allConnected &= listeners.back()->connect (procs.back()->getMidiOut().getName());
        audio.push_back (testsynth::renderGuitar ({ { 0.6, testsynth::guitarChord (names[i]), names[i] },
                                                    { 1.6, testsynth::guitarChord (names[i]), names[i] } }, fs, 2.5,
                                                  0.0, (unsigned) (20 + i)));
    }
    check (allConnected, "each instance has its own connectable port");

    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i)
        threads.emplace_back ([&, i] { run (*procs[(size_t) i], audio[(size_t) i]); });
    for (auto& t : threads) t.join();
    const int64_t until = VirtualMidiOut::nowNs() + 3'500'000'000LL;
    while (VirtualMidiOut::nowNs() < until)
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.05, false);

    bool isolated = true;
    std::string detail;
    for (int i = 0; i < 4; ++i)
    {
        std::set<int> got;
        int balance = 0, foreign = 0;
        for (const auto& e : listeners[(size_t) i]->snapshot())
        {
            if ((e.status & 0x0f) != i) ++foreign;
            const bool on = (e.status & 0xf0) == 0x90 && e.data2 > 0;
            balance += on ? 1 : -1;
            if (on) got.insert (e.data1);
        }
        const auto triad = closeVoicing (names[i]);
        const bool hasTriad = std::includes (got.begin(), got.end(), triad.begin(), triad.end());
        const bool ok = foreign == 0 && balance == 0 && hasTriad;
        isolated &= ok;
        detail += std::string (names[i]) + (ok ? " ok " : " WRONG(foreign=" + std::to_string (foreign)
                                                      + " balance=" + std::to_string (balance) + ") ");
    }
    check (isolated, "no crosstalk between ports; each got its chord with balanced notes (" + detail + ")");
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    testNextLineQuantize();
    testNearestWithPort();
    testTestMode();
    testFixedLength();
    testStateAndPorts();
    testNoAllocationInProcessBlock();
    testPortCloseReleasesNotes();
    testRecoversFromNaN();
    testConcurrentInstances();
    return testutil::finish ("test_plugin");
}
