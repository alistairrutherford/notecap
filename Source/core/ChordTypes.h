// Chord vocabulary and naming. Pure C++, no JUCE dependency.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace notecap
{
    enum class Quality : uint8_t
    {
        Major, Minor, Dom7, Maj7, Min7, Dim, Aug, Sus2, Sus4, HalfDim7,
        Count
    };

    // Groups the user can switch on/off in the UI.
    enum class QualityGroup : uint8_t { Triads, Sevenths, Suspended, DimAug };

    struct QualityInfo
    {
        const char* suffix;
        std::vector<int> intervals;   // semitones above the root, root first
        QualityGroup group;
        float prior;                  // multiplies the match score; < 1 favours simpler chords
    };

    inline const QualityInfo& qualityInfo (Quality q)
    {
        static const std::array<QualityInfo, (size_t) Quality::Count> table {{
            { "",     { 0, 4, 7 },     QualityGroup::Triads,    1.000f },
            { "m",    { 0, 3, 7 },     QualityGroup::Triads,    1.000f },
            { "7",    { 0, 4, 7, 10 }, QualityGroup::Sevenths,  0.970f },
            { "maj7", { 0, 4, 7, 11 }, QualityGroup::Sevenths,  0.970f },
            { "m7",   { 0, 3, 7, 10 }, QualityGroup::Sevenths,  0.970f },
            { "dim",  { 0, 3, 6 },     QualityGroup::DimAug,    0.960f },
            { "aug",  { 0, 4, 8 },     QualityGroup::DimAug,    0.950f },
            { "sus2", { 0, 2, 7 },     QualityGroup::Suspended, 0.965f },
            { "sus4", { 0, 5, 7 },     QualityGroup::Suspended, 0.965f },
            { "m7b5", { 0, 3, 6, 10 }, QualityGroup::DimAug,    0.960f },
        }};
        return table[(size_t) q];
    }

    inline const char* pitchClassName (int pc)
    {
        static const char* names[] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
        return names[((pc % 12) + 12) % 12];
    }

    struct Chord
    {
        int root = -1;                 // pitch class 0..11, or -1 for "no chord"
        Quality quality = Quality::Major;
        int bass = -1;                 // pitch class of the bass if it isn't the root, else -1

        bool isNone() const noexcept { return root < 0; }

        // Same harmony, ignoring the bass note.
        bool sameHarmony (const Chord& o) const noexcept
        {
            if (isNone() || o.isNone()) return isNone() == o.isNone();
            return root == o.root && quality == o.quality;
        }

        bool operator== (const Chord& o) const noexcept
        {
            return sameHarmony (o) && (isNone() || bass == o.bass);
        }
        bool operator!= (const Chord& o) const noexcept { return ! (*this == o); }

        std::string name() const
        {
            if (isNone()) return "N";
            std::string s = pitchClassName (root);
            s += qualityInfo (quality).suffix;
            if (bass >= 0 && bass != root) { s += "/"; s += pitchClassName (bass); }
            return s;
        }

        // Pitch classes of the chord tones (root first).
        std::vector<int> pitchClasses() const
        {
            std::vector<int> pcs;
            if (isNone()) return pcs;
            for (int iv : qualityInfo (quality).intervals)
                pcs.push_back ((root + iv) % 12);
            return pcs;
        }

        // Packs into an int for lock-free transfer to the UI (-1 == none).
        int32_t pack() const noexcept
        {
            if (isNone()) return -1;
            return root | ((int32_t) quality << 4) | ((bass + 1) << 8);
        }

        static Chord unpack (int32_t v) noexcept
        {
            Chord c;
            if (v < 0) return c;
            c.root = v & 0xf;
            c.quality = (Quality) ((v >> 4) & 0xf);
            c.bass = ((v >> 8) & 0x1f) - 1;
            return c;
        }
    };

    // Maps a chord onto the MIREX "majmin" vocabulary (maj / min / N) for scoring.
    // Chords that are neither (dim, aug, sus) map to "X" = excluded.
    inline std::string majMinLabel (const Chord& c)
    {
        if (c.isNone()) return "N";
        switch (c.quality)
        {
            case Quality::Major: case Quality::Dom7: case Quality::Maj7:
                return std::string (pitchClassName (c.root)) + ":maj";
            case Quality::Minor: case Quality::Min7:
                return std::string (pitchClassName (c.root)) + ":min";
            default:
                return "X";
        }
    }
}
