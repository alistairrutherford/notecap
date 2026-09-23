// Turns a detected chord into MIDI note numbers.
#pragma once

#include "ChordTypes.h"
#include <algorithm>
#include <array>

namespace notecap
{
    enum class VoicingStyle { Close, Open, BassAndChord };

    struct Voicing
    {
        static constexpr int maxNotes = 8;
        std::array<int, maxNotes> notes {};
        int count = 0;

        void add (int n) noexcept
        {
            if (count >= maxNotes || n < 0 || n > 127) return;
            for (int i = 0; i < count; ++i) if (notes[(size_t) i] == n) return;
            notes[(size_t) count++] = n;
        }

        const int* begin() const noexcept { return notes.data(); }
        const int* end() const noexcept { return notes.data() + count; }
    };

    // `octave` uses Ableton's naming, where middle C (MIDI 60) is C3.
    inline Voicing makeVoicing (const Chord& chord, VoicingStyle style, int octave) noexcept
    {
        Voicing v;
        if (chord.isNone()) return v;

        const auto& iv = qualityInfo (chord.quality).intervals;
        const int root = 12 * (octave + 2) + chord.root;

        switch (style)
        {
            case VoicingStyle::Close:
                for (int i : iv) v.add (root + i);
                break;

            case VoicingStyle::Open:
                v.add (root);
                v.add (root + iv[2]);          // fifth
                v.add (root + iv[1] + 12);     // third up an octave
                if (iv.size() > 3) v.add (root + iv[3] + 12);
                break;

            case VoicingStyle::BassAndChord:
            {
                const int bassPc = chord.bass >= 0 ? chord.bass : chord.root;
                v.add (12 * (octave + 1) + bassPc);
                for (int i : iv) v.add (root + i);
                break;
            }
        }

        std::sort (v.notes.begin(), v.notes.begin() + v.count);
        return v;
    }
}
