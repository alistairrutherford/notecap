// Template-matching chord classifier over a 12-bin chroma vector.
// Templates model each chord tone's first six harmonics folded into chroma,
// so overtones (e.g. the major 3rd in a note's 5th harmonic) don't cause
// systematic major/minor confusion.
#pragma once

#include "ChordTypes.h"
#include "ChromaExtractor.h"
#include <array>
#include <cmath>

namespace notecap
{
    struct Classification
    {
        Chord chord;
        float score = 0.0f;       // prior-weighted cosine similarity (0..~1)
        float runnerUp = 0.0f;    // best score among chords with a different harmony
    };

    class ChordClassifier
    {
    public:
        static constexpr int numHarmonics = 6;
        static constexpr float harmonicDecay = 0.6f;
        static constexpr float rootEmphasis = 1.15f;
        static constexpr float bassBonus = 0.04f;

        ChordClassifier()
        {
            static const int harmonicSemis[numHarmonics] = { 0, 12, 19, 24, 28, 31 };
            for (int q = 0; q < (int) Quality::Count; ++q)
            {
                const auto& info = qualityInfo ((Quality) q);
                for (int root = 0; root < 12; ++root)
                {
                    Chroma t {};
                    for (size_t i = 0; i < info.intervals.size(); ++i)
                    {
                        const float noteWeight = i == 0 ? rootEmphasis : 1.0f;
                        float w = 1.0f;
                        for (int h = 0; h < numHarmonics; ++h, w *= harmonicDecay)
                            t[(size_t) ((root + info.intervals[i] + harmonicSemis[h]) % 12)] += w * noteWeight;
                    }
                    templates[(size_t) q][(size_t) root] = normalised (t);
                }
            }
            enabled.fill (true);
        }

        void setGroupEnabled (QualityGroup g, bool on) noexcept
        {
            for (int q = 0; q < (int) Quality::Count; ++q)
                if (qualityInfo ((Quality) q).group == g && g != QualityGroup::Triads)
                    enabled[(size_t) q] = on;
        }

        bool isEnabled (Quality q) const noexcept { return enabled[(size_t) q]; }

        // `chroma` need not be normalised. `bass` may be all zeros.
        Classification classify (const Chroma& chroma, const Chroma& bass) const noexcept
        {
            Classification best;
            const Chroma c = normalised (chroma);

            float bassMax = 0.0f;
            for (float b : bass) bassMax = std::max (bassMax, b);

            float scores[(size_t) Quality::Count][12] {};
            for (int q = 0; q < (int) Quality::Count; ++q)
            {
                if (! enabled[(size_t) q]) continue;
                const float prior = qualityInfo ((Quality) q).prior;
                for (int root = 0; root < 12; ++root)
                {
                    const auto& t = templates[(size_t) q][(size_t) root];
                    float dot = 0.0f;
                    for (size_t i = 0; i < 12; ++i) dot += c[i] * t[i];
                    float s = dot * prior;
                    if (bassMax > 0.0f) s += bassBonus * bass[(size_t) root] / bassMax;
                    scores[q][root] = s;
                    if (s > best.score)
                    {
                        best.score = s;
                        best.chord.root = root;
                        best.chord.quality = (Quality) q;
                    }
                }
            }

            if (best.chord.isNone())
                return best;

            for (int q = 0; q < (int) Quality::Count; ++q)
                for (int root = 0; root < 12; ++root)
                    if (enabled[(size_t) q] && ! (root == best.chord.root && (Quality) q == best.chord.quality))
                    {
                        Chord other { root, (Quality) q, -1 };
                        if (! other.sameHarmony (best.chord))
                            best.runnerUp = std::max (best.runnerUp, scores[q][root]);
                    }

            // Slash chord: a clearly dominant bass pitch that is a chord tone but not the root.
            if (bassMax > 0.0f)
            {
                int bassPc = 0;
                for (int i = 1; i < 12; ++i)
                    if (bass[(size_t) i] > bass[(size_t) bassPc]) bassPc = i;

                float second = 0.0f;
                for (int i = 0; i < 12; ++i)
                    if (i != bassPc) second = std::max (second, bass[(size_t) i]);

                if (bassPc != best.chord.root && bass[(size_t) bassPc] > 1.5f * second)
                    for (int pc : best.chord.pitchClasses())
                        if (pc == bassPc) best.chord.bass = bassPc;
            }
            return best;
        }

        // Prior-weighted cosine similarity of `chroma` to one specific chord (no bass bonus).
        float score (const Chroma& chroma, const Chord& chord) const noexcept
        {
            if (chord.isNone()) return 0.0f;
            const Chroma c = normalised (chroma);
            const auto& t = templates[(size_t) chord.quality][(size_t) chord.root];
            float dot = 0.0f;
            for (size_t i = 0; i < 12; ++i) dot += c[i] * t[i];
            return dot * qualityInfo (chord.quality).prior;
        }

        const Chroma& getTemplate (Quality q, int root) const noexcept { return templates[(size_t) q][(size_t) root]; }

    private:
        std::array<std::array<Chroma, 12>, (size_t) Quality::Count> templates {};
        std::array<bool, (size_t) Quality::Count> enabled {};
    };
}
