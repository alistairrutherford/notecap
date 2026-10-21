// Integration test: drives the real NoteCapAudioProcessor with a fake Live
// transport and strummed-guitar input, and checks the MIDI it writes to the
// host buffer (what Live records through "MIDI From: track / NoteCap").
// Run via run_integration.sh.
#include "PluginProcessor.h"
#include "Synth.h"
#include "TestUtil.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <string>
#include <thread>
#include <vector>

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

    struct HostNote { int64_t sample; int channel, note; bool on; };

    void setParam (NoteCapAudioProcessor& p, const char* id, float plainValue)
    {
        auto* param = p.apvts.getParameter (id);
        param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
    }

    // Feeds audio block by block under a fake 120 BPM transport and collects the
    // MIDI the processor writes. Keeps its position, so a test can continue a run.
    struct Host
    {
        NoteCapAudioProcessor& proc;
        FakePlayHead ph;
        juce::AudioBuffer<float> buf { 4, block };   // main L/R + sidechain L/R
        juce::MidiBuffer midi;
        int64_t pos = 0;
        std::vector<HostNote> notes;
        bool toSidechain = false;
        bool countAllocations = false;

        explicit Host (NoteCapAudioProcessor& p) : proc (p)
        {
            midi.ensureSize (4096);   // the host owns this buffer; don't count its growth
            proc.setRateAndBufferSizeDetails (fs, block);
            proc.setPlayHead (&ph);
            proc.prepareToPlay (fs, block);
        }

        ~Host() { proc.setPlayHead (nullptr); }

        void collect (int64_t blockStart)
        {
            for (const auto meta : midi)
            {
                const auto m = meta.getMessage();
                if (m.isNoteOnOrOff())
                    notes.push_back ({ blockStart + meta.samplePosition, m.getChannel(), m.getNoteNumber(), m.isNoteOn() });
            }
        }

        // Plays `audio` (then `extraBlocks` of silence) with the transport `playing`.
        void play (const std::vector<float>& audio, bool playing = true, int extraBlocks = 0)
        {
            const int64_t total = (int64_t) audio.size() + extraBlocks * block;
            for (int64_t i = 0; i < total; i += block)
            {
                ph.info.setIsPlaying (playing);
                ph.info.setBpm (bpm);
                ph.info.setPpqPosition ((double) pos / samplesPerBeat);
                buf.clear();
                const int ch = toSidechain ? 2 : 0;
                for (int k = 0; k < block && i + k < (int64_t) audio.size(); ++k)
                {
                    buf.setSample (ch, k, audio[(size_t) (i + k)]);
                    buf.setSample (ch + 1, k, audio[(size_t) (i + k)]);
                }
                midi.clear();
                alloccount::counting = countAllocations;
                proc.processBlock (buf, midi);
                alloccount::counting = false;
                collect (pos);
                pos += block;
            }
        }

        void stop (int blocks = 4) { play ({}, false, blocks); }

        void bypassedBlock()
        {
            buf.clear();
            midi.clear();
            proc.processBlockBypassed (buf, midi);
            collect (pos);
            pos += block;
        }
    };

    // Groups note-ons by sample position into chords (sorted note lists).
    std::map<int64_t, std::vector<int>> chordsBySample (const std::vector<HostNote>& notes)
    {
        std::map<int64_t, std::vector<int>> out;
        for (const auto& n : notes)
            if (n.on) out[n.sample].push_back (n.note);
        for (auto& [s, v] : out) std::sort (v.begin(), v.end());
        return out;
    }

    // Notes still on at the end, in order of the events.
    std::set<int> sounding (const std::vector<HostNote>& notes, int64_t upTo = std::numeric_limits<int64_t>::max())
    {
        std::set<int> on;
        for (const auto& n : notes)
            if (n.sample <= upTo) { if (n.on) on.insert (n.note); else on.erase (n.note); }
        return on;
    }

    std::vector<int> closeVoicing (const char* name)
    {
        static const std::map<std::string, std::vector<int>> v {
            { "C", { 60, 64, 67 } }, { "Am", { 69, 72, 76 } }, { "F", { 65, 69, 72 } }, { "G", { 67, 71, 74 } },
            { "D", { 62, 66, 69 } } };
        return v.at (name);
    }

    // Strums a few ms either side of beats 2, 4, 6 and 8 (seconds 1..4 at 120 BPM).
    std::vector<testsynth::Strum> strumsAround (double offsetSec)
    {
        return { { 1.0 + offsetSec, testsynth::guitarChord ("C"),  "C" },
                 { 2.0 + offsetSec, testsynth::guitarChord ("Am"), "Am" },
                 { 3.0 + offsetSec, testsynth::guitarChord ("F"),  "F" },
                 { 4.0 + offsetSec, testsynth::guitarChord ("G"),  "G" } };
    }

    const std::vector<std::vector<int>> cAmFG { closeVoicing ("C"), closeVoicing ("Am"), closeVoicing ("F"), closeVoicing ("G") };
    const int64_t strumBeats[] = { 2, 4, 6, 8 };
}

static void testNextLineQuantize()
{
    std::cout << "== next-line quantize ==\n";
    NoteCapAudioProcessor proc;
    setParam (proc, "grid", 3);     // 1/4
    setParam (proc, "qmode", 1);    // next line
    Host host (proc);
    host.play (testsynth::renderGuitar ({ { 1.020, testsynth::guitarChord ("C"),  "C" },
                                          { 1.975, testsynth::guitarChord ("Am"), "Am" },
                                          { 3.010, testsynth::guitarChord ("F"),  "F" },
                                          { 3.985, testsynth::guitarChord ("G"),  "G" } }, fs, 5.0));
    host.stop();
    const auto chords = chordsBySample (host.notes);

    bool allOnGrid = ! chords.empty();
    for (const auto& [s, v] : chords) allOnGrid &= (s % samplesPerBeat) == 0;
    check (allOnGrid, "every chord lands exactly on a beat (" + std::to_string (chords.size()) + " chords)");

    std::vector<std::vector<int>> got;
    for (const auto& [s, v] : chords) got.push_back (v);
    check (got == cAmFG, "chord sequence C Am F G in close voicing");

    if (chords.size() == 4)
    {
        const int64_t expectBeats[] = { 3, 5, 7, 9 };   // strum ~ beat 2 + ~130 ms detection -> beat 3
        int i = 0; bool ok = true;
        for (const auto& [s, v] : chords) ok &= s == expectBeats[i++] * samplesPerBeat;
        check (ok, "next-line mode waits for the first beat after detection");
    }
    check (sounding (host.notes).empty(), "transport stop releases every held note");
}

static void testNearestQuantize()
{
    std::cout << "== nearest quantize ==\n";
    {
        // Strummed 200 ms ahead of the beat: detected before it, so scheduled exactly on it.
        NoteCapAudioProcessor proc;
        setParam (proc, "grid", 3);
        setParam (proc, "qmode", 0);
        Host host (proc);
        host.play (testsynth::renderGuitar (strumsAround (-0.200), fs, 5.0));
        host.stop();
        const auto chords = chordsBySample (host.notes);
        bool onBeat = chords.size() == 4;
        int i = 0;
        std::vector<std::vector<int>> got;
        for (const auto& [s, v] : chords) { if (i < 4) onBeat &= s == strumBeats[i++] * samplesPerBeat; got.push_back (v); }
        check (onBeat, "early strums land exactly on their beat (" + std::to_string (chords.size()) + " chords)");
        check (got == cAmFG, "early strums: C Am F G");
    }
    {
        // Strummed just after the beat: that line has passed by the time the chord is
        // known, so it goes out at once (Live's Record Quantization can snap it).
        NoteCapAudioProcessor proc;
        setParam (proc, "grid", 3);
        setParam (proc, "qmode", 0);
        Host host (proc);
        host.play (testsynth::renderGuitar (strumsAround (0.020), fs, 5.0));
        host.stop();
        const auto chords = chordsBySample (host.notes);
        std::string lateness;
        bool soon = true;
        for (int k = 0; k < 4; ++k)
        {
            const int64_t beat = strumBeats[k] * samplesPerBeat;
            const auto it = chords.lower_bound (beat);
            const bool found = it != chords.end() && it->first < beat + samplesPerBeat;
            const int64_t lateMs = found ? (it->first - beat) * 1000 / (int64_t) fs : -1;
            lateness += std::to_string (lateMs) + " ";
            soon &= found && lateMs > 0 && lateMs < 250;
        }
        check (soon, "late strums go out right after detection (ms after beat: " + lateness + ")");

        // Whatever was revised along the way, the notes held just before each next
        // strum must be the right chord.
        std::vector<std::vector<int>> held;
        for (int k = 0; k < 4; ++k)
        {
            const auto s = sounding (host.notes, (strumBeats[k] + 2) * samplesPerBeat - 1);
            held.emplace_back (s.begin(), s.end());
        }
        check (held == cAmFG, "held notes settle on C, Am, F, G");
    }
}

static void testSources()
{
    std::cout << "== input source ==\n";
    const auto audio = testsynth::renderGuitar ({ { 0.5, testsynth::guitarChord ("D"), "D" } }, fs, 1.5);
    NoteCapAudioProcessor main, side, wrong;
    check ((int) main.apvts.getRawParameterValue ("source")->load() == 1, "Source defaults to Main input");
    setParam (main, "grid", 0);
    setParam (side, "grid", 0);
    setParam (side, "source", 0);
    setParam (wrong, "grid", 0);   // listens to Main, audio only on the sidechain

    Host hm (main);
    hm.play (audio);
    Host hs (side);
    hs.toSidechain = true;
    hs.play (audio);
    Host hw (wrong);
    hw.toSidechain = true;
    hw.play (audio);

    const auto d = closeVoicing ("D");
    check (sounding (hm.notes) == std::set<int> (d.begin(), d.end()), "Main input: detects D");
    check (sounding (hs.notes) == std::set<int> (d.begin(), d.end()), "Sidechain: detects D");
    check (hw.notes.empty(), "Main input ignores audio on the sidechain");
}

static void testTestMode()
{
    std::cout << "== test-chord mode ==\n";
    NoteCapAudioProcessor proc;
    setParam (proc, "grid", 4);     // 1/8
    setParam (proc, "testMode", 1);
    Host host (proc);
    host.play (std::vector<float> ((size_t) (samplesPerBeat * 4), 0.0f));   // 4 beats = 8 eighths
    host.stop();
    const auto chords = chordsBySample (host.notes);
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
    setParam (proc, "grid", 3);     // 1/4
    setParam (proc, "qmode", 1);
    setParam (proc, "length", 1);   // 1 step
    Host host (proc);
    host.play (testsynth::renderGuitar ({ { 1.0, testsynth::guitarChord ("C"), "C" } }, fs, 3.0));
    int64_t on = -1, off = -1;
    for (const auto& n : host.notes)
    {
        if (n.on && on < 0) on = n.sample;
        if (! n.on && off < 0) off = n.sample;
    }
    check (on >= 0 && off - on == samplesPerBeat, "note-off exactly one grid step after note-on");
}

static void testNoStuckNotes()
{
    std::cout << "== no stuck notes ==\n";
    const auto audio = testsynth::renderGuitar ({ { 0.3, testsynth::guitarChord ("G"), "G" } }, fs, 1.0);
    auto heldChord = [&] (NoteCapAudioProcessor& proc, Host& host)
    {
        setParam (proc, "grid", 0);
        host.play (audio);
        return ! sounding (host.notes).empty();
    };

    {
        NoteCapAudioProcessor proc;
        Host host (proc);
        const bool held = heldChord (proc, host);
        host.bypassedBlock();
        check (held && sounding (host.notes).empty(), "bypass releases the held chord");
    }
    {
        NoteCapAudioProcessor proc;
        Host host (proc);
        const bool held = heldChord (proc, host);
        const auto before = host.notes.size();
        proc.reset();   // VST3 setProcessing (false), e.g. Live switching the device off
        host.play ({}, true, 1);
        bool atStart = host.notes.size() > before;
        for (size_t i = before; i < host.notes.size(); ++i)
            atStart &= ! host.notes[i].on && host.notes[i].sample == host.pos - block;
        check (held && atStart && sounding (host.notes).empty(), "reset(): note-offs at the start of the next block");
    }
    {
        NoteCapAudioProcessor proc;
        Host host (proc);
        const bool held = heldChord (proc, host);
        proc.releaseResources();
        proc.prepareToPlay (fs, block);
        host.play ({}, true, 1);
        check (held && sounding (host.notes).empty(), "releaseResources + prepareToPlay: chord released on the next block");
    }
}

static void testState()
{
    std::cout << "== state ==\n";
    NoteCapAudioProcessor a;
    setParam (a, "grid", 6);
    setParam (a, "octave", 2);
    setParam (a, "source", 0);
    juce::MemoryBlock state;
    a.getStateInformation (state);

    NoteCapAudioProcessor b;
    b.setStateInformation (state.getData(), (int) state.getSize());
    check ((int) b.apvts.getRawParameterValue ("grid")->load() == 6
               && (int) b.apvts.getRawParameterValue ("octave")->load() == 2
               && (int) b.apvts.getRawParameterValue ("source")->load() == 0,
           "parameters survive a state round trip");

    // A set saved by the earlier build still has the virtual port's settings.
    auto xml = a.apvts.copyState().createXml();
    xml->setAttribute ("portIndex", 3);
    for (auto [id, v] : { std::pair<const char*, double> { "virtualPort", 1.0 }, { "offsetMs", 40.0 } })
    {
        auto* p = xml->createNewChildElement ("PARAM");
        p->setAttribute ("id", id);
        p->setAttribute ("value", v);
    }
    juce::MemoryBlock old;
    juce::AudioProcessor::copyXmlToBinary (*xml, old);
    NoteCapAudioProcessor c;
    c.setStateInformation (old.getData(), (int) old.getSize());
    check ((int) c.apvts.getRawParameterValue ("grid")->load() == 6
               && c.apvts.getParameter ("virtualPort") == nullptr,
           "a state from the build with the virtual port still loads");
}

static void testNoAllocationInProcessBlock()
{
    std::cout << "== real-time safety ==\n";
    NoteCapAudioProcessor proc;
    setParam (proc, "grid", 3);
    setParam (proc, "length", 2);   // fixed length: exercises release scheduling too
    const auto audio = testsynth::renderGuitar ({ { 0.5, testsynth::guitarChord ("C"), "C" },
                                                  { 1.5, testsynth::guitarChord ("Am"), "Am" },
                                                  { 2.5, testsynth::guitarChord ("E7"), "E7" },
                                                  { 3.5, testsynth::guitarChord ("G"), "G" } }, fs, 4.5);
    {
        Host warm (proc);   // warm-up
        warm.play (audio);
        warm.stop();
    }
    Host host (proc);
    alloccount::count = 0;
    host.countAllocations = true;
    host.play (audio);
    host.stop (8);
    check (! host.notes.empty() && alloccount::count.load() == 0,
           "no heap allocation inside processBlock (" + std::to_string (alloccount::count.load()) + " over "
               + std::to_string (host.pos / block) + " blocks, " + std::to_string (host.notes.size()) + " MIDI events)");
}

static void testRecoversFromNaN()
{
    std::cout << "== bad input ==\n";
    NoteCapAudioProcessor proc;
    setParam (proc, "grid", 0);
    auto audio = testsynth::renderGuitar ({ { 0.5, testsynth::guitarChord ("C"), "C" },
                                            { 2.0, testsynth::guitarChord ("G"), "G" } }, fs, 3.0);
    for (size_t i = (size_t) (1.2 * fs); i < (size_t) (1.3 * fs); ++i)
        audio[i] = (i % 3 == 0) ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
    Host host (proc);
    host.play (audio);
    bool sawG = false;
    for (const auto& [s, v] : chordsBySample (host.notes)) sawG |= s > (int64_t) (1.9 * fs) && v == closeVoicing ("G");
    check (sawG, "still detects the next chord after 100 ms of NaN/Inf input");
}

static void testConcurrentInstances()
{
    std::cout << "== four instances processing concurrently ==\n";
    const char* names[] = { "C", "Am", "F", "G" };
    std::vector<std::unique_ptr<NoteCapAudioProcessor>> procs;
    std::vector<std::unique_ptr<Host>> hosts;
    std::vector<std::vector<float>> audio;
    for (int i = 0; i < 4; ++i)
    {
        procs.push_back (std::make_unique<NoteCapAudioProcessor>());
        setParam (*procs.back(), "grid", 3);
        setParam (*procs.back(), "channel", (float) (i + 1));   // tags every message with its instance
        hosts.push_back (std::make_unique<Host> (*procs.back()));
        audio.push_back (testsynth::renderGuitar ({ { 0.6, testsynth::guitarChord (names[i]), names[i] },
                                                    { 1.6, testsynth::guitarChord (names[i]), names[i] } }, fs, 2.5,
                                                  0.0, (unsigned) (20 + i)));
    }

    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i)
        threads.emplace_back ([&, i] { hosts[(size_t) i]->play (audio[(size_t) i]); hosts[(size_t) i]->stop(); });
    for (auto& t : threads) t.join();

    bool isolated = true;
    std::string detail;
    for (int i = 0; i < 4; ++i)
    {
        std::set<int> got;
        int foreign = 0;
        for (const auto& n : hosts[(size_t) i]->notes)
        {
            if (n.channel != i + 1) ++foreign;
            if (n.on) got.insert (n.note);
        }
        const auto triad = closeVoicing (names[i]);
        const bool ok = foreign == 0 && sounding (hosts[(size_t) i]->notes).empty()
                        && std::includes (got.begin(), got.end(), triad.begin(), triad.end());
        isolated &= ok;
        detail += std::string (names[i]) + (ok ? " ok " : " WRONG(foreign=" + std::to_string (foreign) + ") ");
    }
    check (isolated, "no crosstalk between instances; each played its own chord and released it (" + detail + ")");
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    testNextLineQuantize();
    testNearestQuantize();
    testSources();
    testTestMode();
    testFixedLength();
    testNoStuckNotes();
    testState();
    testNoAllocationInProcessBlock();
    testRecoversFromNaN();
    testConcurrentInstances();
    return testutil::finish ("test_plugin");
}
