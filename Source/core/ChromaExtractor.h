// Peak-picked, tuning-corrected chroma + bass chroma from one analysis frame.
#pragma once

#include "Dsp.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>

namespace notecap
{
    using Chroma = std::array<float, 12>;

    class ChromaExtractor
    {
    public:
        static constexpr int lowestNote  = 28;   // E1
        static constexpr int highestNote = 100;  // E7
        static constexpr int bassTop     = 55;   // G3: notes at or below feed the bass chroma
        static constexpr int numNotes    = highestNote - lowestNote + 1;
        using Notes = std::array<float, numNotes>;

        // Assumed amplitude ratio between successive harmonics of a note, used to
        // explain overtones away before folding into chroma (see unmixHarmonics()).
        static constexpr float harmonicShape = 0.6f;
        static constexpr int nnlsIterations = 40;

        ChromaExtractor (double sampleRate, int fftSize)
            : fs (sampleRate), fft (fftSize), mags ((size_t) fftSize / 2)
        {
            // Dictionary: column j is the semitone profile of note j (12 harmonics).
            static const int harmonicSemis[] = { 0, 12, 19, 24, 28, 31, 34, 36, 38, 40, 42, 43 };
            for (int j = 0; j < numNotes; ++j)
            {
                float w = 1.0f;
                for (int off : harmonicSemis)
                {
                    if (j + off < numNotes) dict[(size_t) (j + off)][(size_t) j] = w;
                    w *= harmonicShape;
                }
            }
            for (int i = 0; i < numNotes; ++i)
                for (int j = 0; j < numNotes; ++j)
                {
                    float sum = 0.0f;
                    for (int k = 0; k < numNotes; ++k) sum += dict[(size_t) k][(size_t) i] * dict[(size_t) k][(size_t) j];
                    gram[(size_t) i][(size_t) j] = sum;
                }
        }

        int getFftSize() const noexcept { return fft.size(); }

        struct Result
        {
            Chroma chroma {};       // un-normalised
            Chroma bass {};         // un-normalised
            float tonalEnergy = 0;  // sum of peak amplitudes in range
        };

        // `frame` holds getFftSize() samples, oldest first.
        // `updateTuning`: pass false during silence so noise doesn't drag the estimate.
        Result process (const float* frame, bool updateTuning) noexcept
        {
            fft.process (frame, mags.data());

            const int nBins = fft.numBins();
            const double binHz = fs / fft.size();
            const int kMin = std::max (2, (int) std::floor (noteToHz (lowestNote - 0.5) / binHz));
            const int kMax = std::min (nBins - 2, (int) std::ceil (noteToHz (highestNote + 0.5) / binHz));

            float maxMag = 0.0f;
            for (int k = kMin; k <= kMax; ++k)
                maxMag = std::max (maxMag, mags[(size_t) k]);

            semis.fill (0.0f);
            Result r;
            if (maxMag < 1.0e-5f)
                return r;

            const float floorMag = std::max (1.0e-5f, maxMag * 0.01f); // -40 dB relative
            std::complex<double> phasor (0.0, 0.0);

            for (int k = kMin; k <= kMax; ++k)
            {
                const float m = mags[(size_t) k];
                if (m < floorMag || m <= mags[(size_t) k - 1] || m < mags[(size_t) k + 1])
                    continue;

                // Parabolic interpolation on log magnitude for a sub-bin frequency.
                const double a = std::log (mags[(size_t) k - 1] + 1e-12);
                const double b = std::log ((double) m);
                const double c = std::log (mags[(size_t) k + 1] + 1e-12);
                const double denom = a - 2.0 * b + c;
                const double p = std::abs (denom) > 1e-12 ? 0.5 * (a - c) / denom : 0.0;
                const double freq = (k + p) * binHz;
                const double amp = std::exp (b - 0.25 * (a - c) * p);

                const double note = 69.0 + 12.0 * std::log2 (freq / 440.0);
                const double dev = note - std::round (note);
                phasor += amp * amp * std::polar (1.0, 2.0 * pi * dev);

                const int n = (int) std::lround (note - tuning);
                if (n >= lowestNote && n <= highestNote)
                    semis[(size_t) (n - lowestNote)] += (float) amp;
            }

            if (updateTuning && std::abs (phasor) > 0.0)
            {
                // Slow, magnitude-normalised circular average of the peaks' deviation.
                tuningPhasor = 0.97 * tuningPhasor + 0.03 * (phasor / std::abs (phasor));
                tuning = std::arg (tuningPhasor) / (2.0 * pi);
            }

            unmixHarmonics();
            fold (notes, r);
            return r;
        }

        // Chroma of `notes` minus `scale` x `baseline` (clamped at zero): used to take
        // the previous chord's still-ringing notes out of a new chord's analysis.
        Result foldWithout (const Notes& baseline, float scale) const noexcept
        {
            Notes diff {};
            for (int i = 0; i < numNotes; ++i)
                diff[(size_t) i] = std::max (0.0f, notes[(size_t) i] - scale * baseline[(size_t) i]);
            Result r;
            fold (diff, r);
            return r;
        }

        // Current tuning offset from A440 in semitones (-0.5 .. 0.5).
        double getTuning() const noexcept { return tuning; }
        void resetTuning() noexcept { tuning = 0.0; tuningPhasor = { 1.0, 0.0 }; }

        const std::array<float, numNotes>& getSemitoneSpectrum() const noexcept { return semis; }
        const std::array<float, numNotes>& getNoteActivations() const noexcept { return notes; }

    private:
        // Non-negative least squares fit of the semitone spectrum to note profiles
        // (multiplicative updates), so a note's overtones are attributed to that
        // note instead of showing up as extra pitch classes. The approach follows
        // Mauch & Dixon's NNLS chroma (ISMIR 2010); this is an independent implementation.
        void unmixHarmonics() noexcept
        {
            std::array<float, numNotes> atb {};
            for (int j = 0; j < numNotes; ++j)
            {
                float sum = 0.0f;
                for (int k = j; k < numNotes; ++k) sum += dict[(size_t) k][(size_t) j] * semis[(size_t) k];
                atb[(size_t) j] = sum;
                notes[(size_t) j] = semis[(size_t) j];
            }

            for (int it = 0; it < nnlsIterations; ++it)
                for (int j = 0; j < numNotes; ++j)
                {
                    if (notes[(size_t) j] <= 0.0f || atb[(size_t) j] <= 0.0f) { notes[(size_t) j] = 0.0f; continue; }
                    float denom = 1.0e-9f;
                    const auto& g = gram[(size_t) j];
                    for (int k = 0; k < numNotes; ++k) denom += g[(size_t) k] * notes[(size_t) k];
                    notes[(size_t) j] *= atb[(size_t) j] / denom;
                }
        }

        static void fold (const Notes& act, Result& r) noexcept
        {
            for (int i = 0; i < numNotes; ++i)
            {
                const float s = act[(size_t) i];
                if (s <= 0.0f) continue;
                r.tonalEnergy += s;

                const int note = lowestNote + i;
                const float l = std::log1p (300.0f * s);
                r.chroma[(size_t) (note % 12)] += l * noteWeight (note);
                if (note <= bassTop)
                    r.bass[(size_t) (note % 12)] += l * (1.0f - 0.6f * (float) (note - lowestNote) / (float) (bassTop - lowestNote));
            }
        }

        static double noteToHz (double note) { return 440.0 * std::pow (2.0, (note - 69.0) / 12.0); }

        // Chroma is built from the range where chord fundamentals live. Above ~C5 the
        // spectrum is dominated by high-order partials (7th, 9th, 11th harmonics land on
        // b7, 9 and #11), which would read as extended chords.
        static float noteWeight (int note) noexcept
        {
            if (note < 40) return 0.7f;                               // below E2
            if (note > 72) return std::max (0.0f, 1.0f - (float) (note - 72) / 12.0f);
            return 1.0f;
        }

        double fs;
        MagnitudeFft fft;
        std::vector<float> mags;
        std::array<float, numNotes> semis {}, notes {};
        std::array<std::array<float, numNotes>, numNotes> dict {}, gram {};
        double tuning = 0.0;
        std::complex<double> tuningPhasor { 1.0, 0.0 };
    };

    inline Chroma normalised (const Chroma& c) noexcept
    {
        float sum = 0.0f;
        for (float v : c) sum += v * v;
        Chroma out {};
        if (sum <= 0.0f) return out;
        const float inv = 1.0f / std::sqrt (sum);
        for (size_t i = 0; i < 12; ++i) out[i] = c[i] * inv;
        return out;
    }
}
