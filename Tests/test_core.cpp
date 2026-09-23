// Unit tests for chord naming, the classifier, the quantiser and voicing.
#include "TestUtil.h"
#include "core/ChordClassifier.h"
#include "core/Quantizer.h"
#include "core/Voicing.h"
#include <cmath>

using namespace notecap;
using testutil::check;

static void testNaming()
{
    std::cout << "== naming ==\n";
    check (Chord { 9, Quality::Min7, 7 }.name() == "Am7/G", "Am7/G");
    check (Chord { 10, Quality::Major, -1 }.name() == "Bb", "Bb");
    check (Chord {}.name() == "N", "no chord is N");

    bool roundTrip = true;
    for (int q = 0; q < (int) Quality::Count; ++q)
        for (int r = 0; r < 12; ++r)
            for (int b = -1; b < 12; ++b)
            {
                Chord c { r, (Quality) q, b };
                roundTrip &= Chord::unpack (c.pack()) == c;
            }
    check (roundTrip && Chord::unpack (-1).isNone(), "pack/unpack round-trips every chord");
    check (majMinLabel (Chord { 2, Quality::Min7, -1 }) == "D:min", "Dm7 scores as D:min");
}

// Chroma of an idealised instrument: each note with decaying harmonics.
static Chroma renderChroma (const std::vector<int>& midiNotes)
{
    static const int harm[] = { 0, 12, 19, 24, 28, 31, 34, 36 };
    Chroma c {};
    for (int n : midiNotes)
    {
        float w = 1.0f;
        for (int h : harm) { c[(size_t) ((n + h) % 12)] += std::log1p (100.0f * w); w *= 0.55f; }
    }
    return c;
}

static void testClassifier()
{
    std::cout << "== classifier ==\n";
    ChordClassifier cls;
    cls.setGroupEnabled (QualityGroup::Suspended, true);
    cls.setGroupEnabled (QualityGroup::DimAug, true);

    int correct = 0, total = 0;
    std::string firstMiss;
    for (int q = 0; q < (int) Quality::Count; ++q)
        for (int root = 0; root < 12; ++root)
        {
            const Chord truth { root, (Quality) q, -1 };
            std::vector<int> notes;
            for (int iv : qualityInfo ((Quality) q).intervals) notes.push_back (48 + root + iv);
            // sus2/sus4 and aug share pitch-class sets across roots (Csus2 == Gsus4),
            // so, as on a real instrument, the root is also sounded in the bass.
            Chroma bass {}; bass[(size_t) root] = 1.0f;
            const auto result = cls.classify (renderChroma (notes), bass);
            ++total;
            if (result.chord.sameHarmony (truth)) ++correct;
            else if (firstMiss.empty()) firstMiss = truth.name() + " -> " + result.chord.name();
        }
    check (correct == total, "all " + std::to_string (total) + " root-position chords with root bass ("
                                 + std::to_string (correct) + " correct" + (firstMiss.empty() ? "" : ", e.g. " + firstMiss) + ")");

    // Triads-only vocabulary: a C7 should still read as C.
    ChordClassifier triads;
    triads.setGroupEnabled (QualityGroup::Sevenths, false);
    triads.setGroupEnabled (QualityGroup::DimAug, false);
    Chroma cBass {}; cBass[0] = 1.0f;
    check (triads.classify (renderChroma ({ 48, 52, 55, 58 }), cBass).chord.sameHarmony ({ 0, Quality::Major, -1 }),
           "C7 with sevenths disabled -> C");

    // Bass note -> slash chord.
    Chroma bass {}; bass[4] = 1.0f;
    const auto slash = cls.classify (renderChroma ({ 40, 48, 52, 55, 60 }), bass);
    check (slash.chord.name() == "C/E", "C with E in the bass -> C/E (got " + slash.chord.name() + ")");

    check (cls.classify (renderChroma ({ 57, 60, 64 }), {}).runnerUp < cls.classify (renderChroma ({ 57, 60, 64 }), {}).score,
           "runner-up score below the winner");
}

static void testQuantizer()
{
    std::cout << "== quantizer ==\n";
    QuantizeGrid g; g.stepPpq = 1.0;

    check (std::abs (g.nextLine (2.3) - 3.0) < 1e-9, "next line after 2.3 is 3");
    check (std::abs (g.nextLine (3.0) - 3.0) < 1e-9, "next line at 3.0 is 3");
    check (std::abs (g.nearestLine (2.4) - 2.0) < 1e-9, "nearest to 2.4 is 2");
    check (std::abs (g.nearestLine (2.6) - 3.0) < 1e-9, "nearest to 2.6 is 3");

    // Onset just before the beat, detected before the beat: wait for the beat.
    auto d = quantize (3.9, 3.95, QuantizeMode::Nearest, g);
    check (std::abs (d.fireAtPpq - 4.0) < 1e-9 && ! d.late, "nearest: early strum waits for the beat");

    // Onset just before the beat but detected after it: fire now, stamped on the beat.
    d = quantize (3.9, 4.2, QuantizeMode::Nearest, g);
    check (d.late && std::abs (d.fireAtPpq - 4.2) < 1e-9 && std::abs (d.stampPpq - 4.0) < 1e-9,
           "nearest: late detection fires now, stamped at the beat");

    d = quantize (3.9, 4.2, QuantizeMode::Next, g);
    check (std::abs (d.fireAtPpq - 5.0) < 1e-9, "next: always the next line after now");

    d = quantize (3.9, 4.2, QuantizeMode::Off, g);
    check (std::abs (d.fireAtPpq - 4.2) < 1e-9, "off: fire immediately");

    QuantizeGrid sw; sw.stepPpq = 0.5; sw.swing = 1.0 / 3.0; // 66% swing on 1/8ths
    check (std::abs (sw.line (1) - (0.5 + 1.0 / 12.0)) < 1e-9, "swung odd 1/8 is delayed");
    check (std::abs (sw.line (2) - 1.0) < 1e-9, "even lines are not swung");
    check (std::abs (sw.nextLine (0.51) - (0.5 + 1.0 / 12.0)) < 1e-9, "next line respects swing");

    // Negative song positions (pre-roll) still land on grid lines.
    check (std::abs (g.nextLine (-0.5) - 0.0) < 1e-9, "pre-roll next line");
}

static void testVoicing()
{
    std::cout << "== voicing ==\n";
    const Chord am7 { 9, Quality::Min7, -1 };
    auto v = makeVoicing (am7, VoicingStyle::Close, 3);
    check (v.count == 4 && v.notes[0] == 69 && v.notes[1] == 72 && v.notes[2] == 76 && v.notes[3] == 79,
           "Am7 close, octave 3 -> 69 72 76 79");

    v = makeVoicing (Chord { 0, Quality::Major, 4 }, VoicingStyle::BassAndChord, 3);
    check (v.count == 4 && v.notes[0] == 52 && v.notes[1] == 60, "C/E bass+chord puts E2 (52) under C3");

    v = makeVoicing (Chord { 0, Quality::Major, -1 }, VoicingStyle::Open, 3);
    check (v.count == 3 && v.notes[0] == 60 && v.notes[1] == 67 && v.notes[2] == 76, "C open -> 60 67 76");

    check (makeVoicing (Chord {}, VoicingStyle::Close, 3).count == 0, "no chord -> no notes");
}

int main()
{
    testNaming();
    testClassifier();
    testQuantizer();
    testVoicing();
    return testutil::finish ("test_core");
}
