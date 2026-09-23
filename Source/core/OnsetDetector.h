// Log-spectral-flux onset detector with an adaptive threshold.
#pragma once

#include "Dsp.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace notecap
{
    class OnsetDetector
    {
    public:
        static constexpr int fftSize = 1024;

        explicit OnsetDetector (double sampleRate)
            : fs (sampleRate), fft (fftSize),
              mags ((size_t) fftSize / 2), prevLog ((size_t) fftSize / 2, 0.0f)
        {
            const double binHz = fs / fftSize;
            kMin = std::max (1, (int) (40.0 / binHz));
            kMax = std::min (fftSize / 2 - 1, (int) (5000.0 / binHz));
        }

        // Call once per hop with the latest fftSize samples.
        // Returns true if the *previous* frame was an onset peak (one frame of look-ahead).
        bool process (const float* frame, bool voiced) noexcept
        {
            fft.process (frame, mags.data());

            float flux = 0.0f;
            for (int k = kMin; k <= kMax; ++k)
            {
                const float l = std::log1p (100.0f * mags[(size_t) k]);
                flux += std::max (0.0f, l - prevLog[(size_t) k]);
                prevLog[(size_t) k] = l;
            }
            flux /= (float) (kMax - kMin + 1);

            // Adaptive threshold from recent history (excluding the candidate).
            float mean = 0.0f, var = 0.0f;
            for (float h : history) mean += h;
            mean /= (float) history.size();
            for (float h : history) var += (h - mean) * (h - mean);
            const float sd = std::sqrt (var / (float) history.size());
            const float threshold = std::max (minFlux, mean + sensitivity * sd);

            const bool isPeak = prev1 > threshold && prev1 >= prev2 && prev1 >= flux;
            const bool onset = isPeak && prevVoiced && framesSinceOnset >= minGapFrames;

            history[histPos] = prev1;
            histPos = (histPos + 1) % history.size();
            prev2 = prev1;
            prev1 = flux;
            ++framesSinceOnset;
            if (onset) framesSinceOnset = 1;
            prevVoiced = voiced;
            lastFlux = flux;
            return onset;
        }

        void setMinGapFrames (int frames) noexcept { minGapFrames = std::max (1, frames); }
        float getLastFlux() const noexcept { return lastFlux; }

        void reset() noexcept
        {
            std::fill (prevLog.begin(), prevLog.end(), 0.0f);
            history.fill (0.0f);
            prev1 = prev2 = 0.0f;
            framesSinceOnset = 1000;
            prevVoiced = false;
        }

    private:
        double fs;
        MagnitudeFft fft;
        std::vector<float> mags, prevLog;
        int kMin = 1, kMax = 1;
        std::array<float, 24> history {};
        size_t histPos = 0;
        float prev1 = 0, prev2 = 0, lastFlux = 0;
        int framesSinceOnset = 1000, minGapFrames = 6;
        bool prevVoiced = false;
        static constexpr float sensitivity = 2.0f;
        static constexpr float minFlux = 0.05f;
    };
}
