// Integration test: drives the real NoteCapAudioProcessor with a fake Live
// transport and a strummed-guitar sidechain, and listens on its CoreMIDI
// virtual port like Ableton would. Run via run_integration.sh.
#include "PluginProcessor.h"
#include "Synth.h"
#include "TestUtil.h"
#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>
#include <map>
#include <mutex>
#include <set>

using testutil::check;

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
    RunResult run (NoteCapAudioProcessor& proc, const std::vector<float>& sidechain, int stoppedBlocks = 8)
    {
        FakePlayHead ph;
        proc.setPlayHead (&ph);
        proc.prepareToPlay (fs, block);

        RunResult r;
        r.hostStartNs = VirtualMidiOut::nowNs() + 100'000'000;  // a little in the future
        juce::AudioBuffer<float> buf (4, block);   // main L/R + sidechain L/R
        juce::MidiBuffer midi;

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
            proc.processBlock (buf, midi);
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

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    testNextLineQuantize();
    testNearestWithPort();
    testTestMode();
    testFixedLength();
    testStateAndPorts();
    return testutil::finish ("test_plugin");
}
