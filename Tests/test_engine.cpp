// End-to-end tests of ChordEngine on synthetic strummed guitar and sustained tones.
#include "Synth.h"
#include "TestUtil.h"
#include "core/ChordEngine.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>

using namespace notecap;
using testutil::check;

namespace
{
    struct Segment
    {
        uint32_t id;
        Chord chord;
        double onsetSec, firstDetectSec;
    };

    int correctionCount = 0;

    std::vector<Segment> runEngine (const std::vector<float>& audio, double fs,
                                    ChordEngine::Params p = {}, int block = 256)
    {
        ChordEngine engine;
        engine.prepare (fs);
        engine.setParams (p);

        std::vector<Segment> segs;
        ChordEventList events;
        for (size_t pos = 0; pos < audio.size(); pos += (size_t) block)
        {
            const int n = (int) std::min ((size_t) block, audio.size() - pos);
            events.clear();
            engine.process (audio.data() + pos, n, (int64_t) pos, events);
            for (const auto& e : events)
            {
                if (e.correction) ++correctionCount;
                auto it = std::find_if (segs.begin(), segs.end(), [&] (const Segment& s) { return s.id == e.segment; });
                if (it != segs.end()) it->chord = e.chord;   // correction
                else segs.push_back ({ e.segment, e.chord, e.onsetSample / fs, e.detectSample / fs });
            }
        }
        return segs;
    }

    struct Score { int matched = 0, matchedMajMin = 0, total = 0; double meanLatencyMs = 0, maxLatencyMs = 0, maxOnsetErrMs = 0; bool sequenceOk = false; };

    Chord parseChord (const std::string& name)
    {
        for (int q = 0; q < (int) Quality::Count; ++q)
            for (int r = 0; r < 12; ++r)
            {
                Chord c { r, (Quality) q, -1 };
                if (c.name() == name) return c;
            }
        return {};
    }

    Score evaluate (const std::vector<testsynth::Strum>& truth, const std::vector<Segment>& segs, bool verbose)
    {
        Score sc;
        sc.total = (int) truth.size();

        // Emitted harmony changes (no-chord events dropped).
        std::vector<Segment> changes;
        for (const auto& s : segs)
            if (! s.chord.isNone()) changes.push_back (s);

        std::vector<std::string> expectSeq, gotSeq;
        for (size_t i = 0; i < truth.size(); ++i)
            if (i == 0 || truth[i].label != truth[i - 1].label) expectSeq.push_back (truth[i].label);
        for (const auto& s : changes)
        {
            Chord h = s.chord; h.bass = -1;
            if (gotSeq.empty() || gotSeq.back() != h.name()) gotSeq.push_back (h.name());
        }
        sc.sequenceOk = expectSeq == gotSeq;

        double latSum = 0;
        for (const auto& t : truth)
        {
            const Segment* best = nullptr;
            for (const auto& s : changes)
                if (s.onsetSec > t.timeSec - 0.15 && s.onsetSec < t.timeSec + 0.3
                    && (! best || std::abs (s.onsetSec - t.timeSec) < std::abs (best->onsetSec - t.timeSec)))
                    best = &s;
            if (! best) continue;
            Chord h = best->chord; h.bass = -1;
            if (majMinLabel (h) == majMinLabel (parseChord (t.label))) ++sc.matchedMajMin;
            if (h.name() != t.label) continue;
            ++sc.matched;
            const double lat = (best->firstDetectSec - t.timeSec) * 1000.0;
            latSum += lat;
            sc.maxLatencyMs = std::max (sc.maxLatencyMs, lat);
            sc.maxOnsetErrMs = std::max (sc.maxOnsetErrMs, std::abs (best->onsetSec - t.timeSec) * 1000.0);
        }
        sc.meanLatencyMs = sc.matched ? latSum / sc.matched : 0;

        if (verbose)
        {
            std::printf ("     expected:");
            for (auto& s : expectSeq) std::printf (" %s", s.c_str());
            std::printf ("\n     got:     ");
            for (auto& s : gotSeq) std::printf (" %s", s.c_str());
            std::printf ("\n");
            for (const auto& s : segs)
                std::printf ("       seg %3u  %-8s onset %7.3fs  detect %7.3fs\n", s.id, s.chord.name().c_str(), s.onsetSec, s.firstDetectSec);
        }
        return sc;
    }

    std::vector<testsynth::Strum> progression (const std::vector<std::string>& names, double start, double spacing,
                                               double jitterMs = 0.0, unsigned seed = 3)
    {
        std::mt19937 rng (seed);
        std::uniform_real_distribution<double> j (-jitterMs, jitterMs);
        std::vector<testsynth::Strum> out;
        double t = start;
        for (const auto& n : names)
        {
            out.push_back ({ t + j (rng) * 0.001, testsynth::guitarChord (n), n });
            t += spacing;
        }
        return out;
    }

    void reportScore (const std::string& name, const Score& sc)
    {
        char buf[256];
        std::snprintf (buf, sizeof buf, "%s: majmin %d/%d, exact %d/%d, latency mean %.0f ms / max %.0f ms, onset err max %.0f ms",
                       name.c_str(), sc.matchedMajMin, sc.total, sc.matched, sc.total,
                       sc.meanLatencyMs, sc.maxLatencyMs, sc.maxOnsetErrMs);
        check (sc.matchedMajMin == sc.total, buf);
    }
}

static void testGuitarProgression (double fs)
{
    std::cout << "== strummed guitar @ " << fs << " Hz ==\n";
    const std::vector<std::string> names { "C", "Am", "F", "G", "Em", "D", "A", "E", "Dm", "Bb", "Bm", "C" };
    const auto truth = progression (names, 0.5, 1.0, 25.0);
    const auto audio = testsynth::renderGuitar (truth, fs, 0.5 + names.size() * 1.0 + 0.5);
    const auto segs = runEngine (audio, fs);
    const auto sc = evaluate (truth, segs, false);
    reportScore ("triads", sc);
    check (sc.maxLatencyMs < 250.0, "every chord decided within 250 ms");
    check (sc.maxOnsetErrMs < 50.0, "onset estimated within 50 ms");
}

static void testSevenths()
{
    std::cout << "== sevenths ==\n";
    const double fs = 44100;
    const std::vector<std::string> names { "Cmaj7", "Dm7", "E7", "Am", "A7", "Dm7", "Bm7", "E7" };
    const auto truth = progression (names, 0.5, 1.2);
    const auto audio = testsynth::renderGuitar (truth, fs, 0.5 + names.size() * 1.2 + 0.5, 0.0, 7);
    reportScore ("sevenths", evaluate (truth, runEngine (audio, fs), false));
}

static void testDetuned()
{
    std::cout << "== detuned guitar (+35 cents) ==\n";
    const double fs = 48000;
    const std::vector<std::string> names { "G", "D", "Em", "C", "G", "D", "C", "G" };
    const auto truth = progression (names, 0.5, 1.0);
    const auto audio = testsynth::renderGuitar (truth, fs, 9.0, 35.0, 11);

    ChordEngine engine;
    engine.prepare (fs);
    ChordEventList ev;
    for (size_t p = 0; p < audio.size(); p += 512)
    {
        ev.clear();
        engine.process (audio.data() + p, (int) std::min<size_t> (512, audio.size() - p), (int64_t) p, ev);
    }
    const double cents = engine.getTuningCents();
    check (std::abs (cents - 35.0) < 10.0, "tuning estimate " + std::to_string ((int) std::lround (cents)) + " c ~ +35 c");
    reportScore ("detuned", evaluate (truth, runEngine (audio, fs), false));
}

static void testRestrumAndRelease()
{
    std::cout << "== re-strum and release ==\n";
    const double fs = 44100;
    std::vector<testsynth::Strum> truth { { 0.5, testsynth::guitarChord ("G"), "G" },
                                          { 1.0, testsynth::guitarChord ("G"), "G" },
                                          { 1.5, testsynth::guitarChord ("G"), "G" } };
    auto audio = testsynth::renderGuitar (truth, fs, 2.0);
    audio.resize ((size_t) (fs * 6.0), 0.0f);  // silence after 2 s: the damped strings die away

    auto segs = runEngine (audio, fs);
    int gCount = 0, noneCount = 0;
    for (auto& s : segs) { if (s.chord.isNone()) ++noneCount; else ++gCount; }
    check (gCount == 1, "same chord strummed 3x -> one event without retrigger (got " + std::to_string (gCount) + ")");
    check (noneCount == 1 && segs.back().chord.isNone(), "silence releases the chord");

    ChordEngine::Params p; p.retrigger = true;
    segs = runEngine (audio, fs, p);
    gCount = 0;
    for (auto& s : segs) if (! s.chord.isNone()) ++gCount;
    check (gCount == 3, "retrigger on -> three events (got " + std::to_string (gCount) + ")");
}

static void testSustainedLegato()
{
    std::cout << "== sustained tones, legato changes ==\n";
    const double fs = 44100;
    std::vector<float> audio ((size_t) (fs * 7.0), 0.0f);
    // Pad-like: each chord's notes start slightly staggered and overlap the next chord.
    const std::vector<std::pair<std::string, std::vector<int>>> chords {
        { "C", { 48, 60, 64, 67 } }, { "Am", { 45, 60, 64, 69 } }, { "F", { 41, 60, 65, 69 } }, { "G", { 43, 59, 62, 67 } } };
    std::vector<testsynth::Strum> truth;
    for (size_t i = 0; i < chords.size(); ++i)
    {
        const double t = 0.5 + 1.5 * (double) i;
        truth.push_back ({ t, {}, chords[i].first });
        for (int n : chords[i].second) testsynth::addTone (audio, fs, t, 1.55, n, 0.05f);
    }
    reportScore ("pad", evaluate (truth, runEngine (audio, fs), false));
}

static void testBlockSizeIndependence()
{
    std::cout << "== block size independence ==\n";
    const double fs = 44100;
    const auto truth = progression ({ "C", "G", "Am", "F" }, 0.3, 0.9);
    const auto audio = testsynth::renderGuitar (truth, fs, 4.0);
    const auto a = runEngine (audio, fs, {}, 32);
    const auto b = runEngine (audio, fs, {}, 2048);
    bool same = a.size() == b.size();
    for (size_t i = 0; same && i < a.size(); ++i)
        same = a[i].chord == b[i].chord && a[i].onsetSec == b[i].onsetSec && a[i].firstDetectSec == b[i].firstDetectSec;
    check (same, "32- and 2048-sample blocks give identical events");
}

// Randomised benchmark: many progressions over the chord library at several
// sample rates, tunings and strum speeds. Returns {exact accuracy, majmin accuracy}.
static std::pair<double, double> benchmark (int numSongs, unsigned seedBase, bool verbose)
{
    static const std::vector<std::string> library { "C", "Am", "F", "G", "Em", "D", "Dm", "A", "E", "E7",
                                                    "Dm7", "Cmaj7", "Bm7", "A7", "Bb", "Bm" };
    static const double rates[] = { 44100, 48000, 96000 };
    int exact = 0, majmin = 0, total = 0;
    for (int song = 0; song < numSongs; ++song)
    {
        std::mt19937 rng (seedBase + (unsigned) song);
        std::uniform_int_distribution<size_t> pick (0, library.size() - 1);
        std::uniform_real_distribution<double> spacing (0.6, 1.5), detune (-30.0, 30.0), spread (5.0, 30.0);
        const double fs = rates[song % 3];

        std::vector<testsynth::Strum> truth;
        double t = 0.4;
        for (int i = 0; i < 10; ++i)
        {
            std::string name;
            do name = library[pick (rng)]; while (! truth.empty() && name == truth.back().label);
            truth.push_back ({ t, testsynth::guitarChord (name), name });
            t += spacing (rng);
        }
        const auto audio = testsynth::renderGuitar (truth, fs, t + 0.5, detune (rng), seedBase * 7 + (unsigned) song, spread (rng));
        const auto sc = evaluate (truth, runEngine (audio, fs), verbose);
        exact += sc.matched; majmin += sc.matchedMajMin; total += sc.total;
    }
    return { (double) exact / total, (double) majmin / total };
}

static void testBenchmark()
{
    correctionCount = 0;
    std::cout << "== randomised benchmark (30 progressions, 300 strums) ==\n";
    const auto [exact, majmin] = benchmark (30, 1000, false);
    char buf[128];
    std::snprintf (buf, sizeof buf, "majmin accuracy %.1f%% (>= 95%%)", majmin * 100.0);
    check (majmin >= 0.95, buf);
    std::snprintf (buf, sizeof buf, "exact-label accuracy %.1f%% (>= 85%%)", exact * 100.0);
    check (exact >= 0.85, buf);
    std::snprintf (buf, sizeof buf, "decisions revised after emission: %.1f%% of strums (<= 15%%)", correctionCount / 3.0);
    check (correctionCount <= 45, buf);
}

int main (int argc, char** argv)
{
    if (argc > 1 && std::string (argv[1]) == "--bench")
    {
        const int songs = argc > 2 ? std::atoi (argv[2]) : 30;
        const auto [exact, majmin] = benchmark (songs, argc > 3 ? (unsigned) std::atoi (argv[3]) : 1000, false);
        std::printf ("exact %.1f%%  majmin %.1f%%  corrections %.1f%%\n", exact * 100.0, majmin * 100.0,
                     100.0 * correctionCount / (songs * 10));
        return 0;
    }

    testGuitarProgression (44100);
    testGuitarProgression (48000);
    testGuitarProgression (96000);
    testSevenths();
    testDetuned();
    testRestrumAndRelease();
    testSustainedLegato();
    testBlockSizeIndependence();
    testBenchmark();
    return testutil::finish ("test_engine");
}
