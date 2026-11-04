// Tests of NoteTracker (single-notes mode) on synthetic monophonic guitar and tones.
#include "Synth.h"
#include "TestUtil.h"
#include "core/NoteTracker.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace notecap;
using testutil::check;

namespace
{
    struct Detected { int note; double onsetSec, detectSec; };
    struct TrueNote { double timeSec; int note; };

    std::vector<Detected> run (const std::vector<float>& audio, double fs, NoteTracker::Params p = {}, int block = 256,
                               int* offs = nullptr)
    {
        NoteTracker t;
        t.prepare (fs);
        t.setParams (p);
        std::vector<Detected> out;
        NoteEventList ev;
        for (size_t pos = 0; pos < audio.size(); pos += (size_t) block)
        {
            ev.clear();
            t.process (audio.data() + pos, (int) std::min<size_t> ((size_t) block, audio.size() - pos), (int64_t) pos, ev);
            for (const auto& e : ev)
            {
                if (e.note >= 0) out.push_back ({ e.note, e.onsetSample / fs, e.detectSample / fs });
                else if (offs) ++*offs;
            }
        }
        return out;
    }

    // Single picked notes on the Karplus-Strong guitar (each pick damps the previous note).
    std::vector<float> pickedLine (const std::vector<TrueNote>& notes, double fs, double length, double detune = 0.0,
                                   unsigned seed = 1)
    {
        std::vector<testsynth::Strum> s;
        for (const auto& n : notes) s.push_back ({ n.timeSec, { n.note }, "" });
        return testsynth::renderGuitar (s, fs, length, detune, seed, 0.0);
    }

    // A sustained harmonic tone whose pitch follows `midiAt(t)` (phase-continuous, so a pitch
    // change has no attack: a hammer-on or slide). Fades in at `start`, out at `end`.
    template <typename F>
    std::vector<float> glidingTone (double fs, double start, double end, double length, F midiAt)
    {
        std::vector<float> out ((size_t) (length * fs), 0.0f);
        std::array<double, 6> phase {};
        for (size_t i = (size_t) (start * fs); i < (size_t) (end * fs) && i < out.size(); ++i)
        {
            const double t = (double) i / fs;
            const double f = testsynth::midiToHz (midiAt (t));
            const double env = std::min (1.0, (t - start) / 0.01) * std::min (1.0, (end - t) / 0.02);
            double y = 0.0;
            for (int h = 0; h < 6; ++h)
            {
                phase[(size_t) h] += 2.0 * M_PI * f * (h + 1) / fs;
                y += std::sin (phase[(size_t) h]) * std::pow (0.6, h);
            }
            out[i] = (float) (0.2 * env * y);
        }
        return out;
    }

    struct Score { int hit = 0, total = 0, extra = 0; std::vector<double> latMs; };

    Score score (const std::vector<TrueNote>& truth, const std::vector<Detected>& got, bool verbose = false)
    {
        Score s;
        s.total = (int) truth.size();
        std::vector<bool> used (got.size(), false);
        for (const auto& t : truth)
        {
            int best = -1;
            for (size_t i = 0; i < got.size(); ++i)
                if (! used[i] && got[i].note == t.note && std::abs (got[i].onsetSec - t.timeSec) < 0.08
                    && (best < 0 || std::abs (got[i].onsetSec - t.timeSec) < std::abs (got[(size_t) best].onsetSec - t.timeSec)))
                    best = (int) i;
            if (best < 0) continue;
            used[(size_t) best] = true;
            ++s.hit;
            s.latMs.push_back ((got[(size_t) best].detectSec - t.timeSec) * 1000.0);
        }
        for (bool u : used) s.extra += u ? 0 : 1;
        if (verbose)
        {
            std::printf ("     truth:");
            for (auto& t : truth) std::printf (" %s@%.2f", noteName (t.note).c_str(), t.timeSec);
            std::printf ("\n     got:  ");
            for (auto& g : got) std::printf (" %s@%.2f", noteName (g.note).c_str(), g.onsetSec);
            std::printf ("\n");
        }
        return s;
    }

    double median (std::vector<double> v)
    {
        if (v.empty()) return 0.0;
        std::sort (v.begin(), v.end());
        return v[v.size() / 2];
    }
}

// Random single-note phrases across a guitar's range. Returns {recall, precision, median latency}.
static std::array<double, 4> benchmark (int phrases, unsigned seedBase, bool verbose)
{
    static const double rates[] = { 44100, 48000, 96000 };
    int hit = 0, total = 0, extra = 0;
    std::vector<double> lat;
    for (int k = 0; k < phrases; ++k)
    {
        std::mt19937 rng (seedBase + (unsigned) k);
        std::uniform_int_distribution<int> pitch (40, 76);           // E2 .. E5
        std::uniform_real_distribution<double> dur (0.18, 0.6), detune (-30.0, 30.0);
        const double fs = rates[k % 3];
        std::vector<TrueNote> truth;
        double t = 0.3;
        for (int i = 0; i < 12; ++i)
        {
            int n;
            do n = pitch (rng); while (! truth.empty() && n == truth.back().note);
            truth.push_back ({ t, n });
            t += dur (rng);
        }
        const auto audio = pickedLine (truth, fs, t + 0.6, detune (rng), seedBase * 3 + (unsigned) k);
        const auto s = score (truth, run (audio, fs), verbose);
        hit += s.hit; total += s.total; extra += s.extra;
        lat.insert (lat.end(), s.latMs.begin(), s.latMs.end());
    }
    return { (double) hit / total, (double) hit / std::max (1, hit + extra), median (lat),
             lat.empty() ? 0.0 : *std::max_element (lat.begin(), lat.end()) };
}

static void testBenchmark()
{
    std::cout << "== randomised single-note benchmark (30 phrases, 360 notes) ==\n";
    const auto [recall, precision, medLat, maxLat] = benchmark (30, 200, false);
    char buf[160];
    std::snprintf (buf, sizeof buf, "recall %.1f%% (>= 95%%)", recall * 100.0);
    check (recall >= 0.95, buf);
    std::snprintf (buf, sizeof buf, "precision %.1f%% (>= 95%%)", precision * 100.0);
    check (precision >= 0.95, buf);
    std::snprintf (buf, sizeof buf, "latency median %.0f ms (< 100), max %.0f ms", medLat, maxLat);
    check (medLat < 100.0, buf);
}

static void testRepeatedNote()
{
    std::cout << "== re-picked note ==\n";
    const double fs = 48000;
    std::vector<TrueNote> truth { { 0.3, 57 }, { 0.7, 57 }, { 1.1, 57 }, { 1.5, 57 } };
    const auto audio = pickedLine (truth, fs, 2.2);
    const auto s = score (truth, run (audio, fs));
    check (s.hit == 4 && s.extra == 0, "A2 (MIDI 57) picked 4 times -> 4 notes (" + std::to_string (s.hit) + " hit, "
                                         + std::to_string (s.extra) + " extra)");
    NoteTracker::Params p; p.retrigger = false;
    check (run (audio, fs, p).size() == 1, "retrigger off -> one note");
}

static void testOctaves()
{
    std::cout << "== octave jumps ==\n";
    const double fs = 44100;
    std::vector<TrueNote> truth { { 0.3, 40 }, { 0.7, 52 }, { 1.1, 64 }, { 1.5, 52 }, { 1.9, 40 }, { 2.3, 76 } };
    const auto s = score (truth, run (pickedLine (truth, fs, 3.0), fs), true);
    check (s.hit == 6 && s.extra == 0, "MIDI 40 52 64 52 40 76 (E1..E4 in Live's names) all right (" + std::to_string (s.hit) + "/6, "
                                         + std::to_string (s.extra) + " extra)");
}

static void testLegato()
{
    std::cout << "== legato changes (hammer-on / slide, no new attack) ==\n";
    const double fs = 44100;
    const auto audio = glidingTone (fs, 0.3, 2.0, 2.4, [] (double t) { return t < 0.9 ? 55.0 : (t < 1.5 ? 57.0 : 59.0); });
    std::vector<TrueNote> truth { { 0.3, 55 }, { 0.9, 57 }, { 1.5, 59 } };
    int offs = 0;
    const auto got = run (audio, fs, {}, 256, &offs);
    const auto s = score (truth, got, true);
    check (s.hit == 3 && s.extra == 0, "G2 -> A2 -> B2 (Live names) without re-picking: 3 notes (" + std::to_string (s.hit) + "/3, "
                                         + std::to_string (s.extra) + " extra)");
    check (offs == 3, "each note ended once, the last at the fade-out (" + std::to_string (offs) + " note-offs)");
}

static void testVibrato()
{
    std::cout << "== vibrato ==\n";
    const double fs = 48000;
    const auto audio = glidingTone (fs, 0.3, 2.0, 2.4, [] (double t) { return 64.0 + 0.3 * std::sin (2.0 * M_PI * 5.5 * t); });
    const auto got = run (audio, fs);
    check (got.size() == 1 && got[0].note == 64, "E3 (MIDI 64) with +/-30 cent vibrato stays one note (" + std::to_string (got.size())
                                                     + " notes)");
}

static void testDetuneAndTuning()
{
    std::cout << "== detuned guitar ==\n";
    const double fs = 44100;
    // +30 cents, and A = 432 Hz (-32 cents). (Near +/-50 cents a note is a quarter tone from two
    // semitones and no tuning estimate can tell which was meant.) The Karplus-Strong strings are
    // themselves ~9 cents sharp, so the tuning check compares the two estimates.
    std::vector<double> estimates;
    for (double cents : { 30.0, -31.8 })
    {
        std::vector<TrueNote> truth;
        for (int i = 0; i < 10; ++i) truth.push_back ({ 0.3 + 0.35 * i, 45 + (i * 5) % 24 });
        const auto audio = pickedLine (truth, fs, 4.2, cents, 9);
        NoteTracker t;
        t.prepare (fs);
        NoteEventList ev;
        std::vector<Detected> got;
        for (size_t pos = 0; pos < audio.size(); pos += 512)
        {
            ev.clear();
            t.process (audio.data() + pos, (int) std::min<size_t> (512, audio.size() - pos), (int64_t) pos, ev);
            for (auto& e : ev) if (e.note >= 0) got.push_back ({ e.note, e.onsetSample / fs, e.detectSample / fs });
        }
        const auto s = score (truth, got);
        const std::string label = (cents > 0 ? "+" : "") + std::to_string ((int) std::lround (cents)) + " cents";
        check (s.hit == 10 && s.extra == 0, label + ": 10/10 notes (" + std::to_string (s.hit) + ", "
                                              + std::to_string (s.extra) + " extra)");
        estimates.push_back (t.getTuningCents());
    }
    const double apart = estimates[0] - estimates[1];
    check (std::abs (apart - 61.8) < 10.0, "tuning estimates " + std::to_string ((int) std::lround (estimates[0])) + " c and "
                                               + std::to_string ((int) std::lround (estimates[1])) + " c: "
                                               + std::to_string ((int) std::lround (apart)) + " c apart (~62)");
}

static void testReleaseAndBlocks()
{
    std::cout << "== release and block sizes ==\n";
    const double fs = 44100;
    std::vector<TrueNote> truth { { 0.3, 52 }, { 0.8, 55 }, { 1.3, 59 } };
    auto audio = pickedLine (truth, fs, 1.8);
    audio.resize ((size_t) (fs * 4.0), 0.0f);   // the last note dies away
    int offs = 0;
    const auto a = run (audio, fs, {}, 32, &offs);
    const auto b = run (audio, fs, {}, 2048);
    check (offs == 3, "every note ends, including the last as it dies away (" + std::to_string (offs) + " note-offs)");
    bool same = a.size() == b.size();
    for (size_t i = 0; same && i < a.size(); ++i)
        same = a[i].note == b[i].note && a[i].onsetSec == b[i].onsetSec && a[i].detectSec == b[i].detectSec;
    check (same, "32- and 2048-sample blocks give identical notes");
    check (noteName (60) == "C3" && noteName (57) == "A2", "note names use Live's octaves (C3 = 60)");
}

int main (int argc, char** argv)
{
    if (argc > 1 && std::string (argv[1]) == "--bench")
    {
        const auto r = benchmark (argc > 2 ? std::atoi (argv[2]) : 30, argc > 3 ? (unsigned) std::atoi (argv[3]) : 200,
                                  argc > 4);
        std::printf ("recall %.1f%%  precision %.1f%%  latency median %.0f ms max %.0f ms\n",
                     r[0] * 100.0, r[1] * 100.0, r[2], r[3]);
        return 0;
    }
    testRepeatedNote();
    testOctaves();
    testLegato();
    testVibrato();
    testDetuneAndTuning();
    testReleaseAndBlocks();
    testBenchmark();
    return testutil::finish ("test_notes");
}
