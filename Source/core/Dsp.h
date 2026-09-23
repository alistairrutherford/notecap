// Small DSP building blocks: real FFT (Accelerate), biquads, decimator.
// Everything allocates in prepare()/constructors only; per-sample calls are RT-safe.
#pragma once

#include <Accelerate/Accelerate.h>
#include <cmath>
#include <vector>

namespace notecap
{
    constexpr double pi = 3.14159265358979323846;

    // Magnitude spectrum of a Hann-windowed real frame, normalised so a
    // full-scale sine produces a peak of ~1.0.
    class MagnitudeFft
    {
    public:
        explicit MagnitudeFft (int size) : n (size)
        {
            log2n = (vDSP_Length) std::lround (std::log2 ((double) n));
            setup = vDSP_create_fftsetup (log2n, FFT_RADIX2);
            window.resize ((size_t) n);
            for (int i = 0; i < n; ++i)
                window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * pi * i / n));
            work.resize ((size_t) n);
            re.resize ((size_t) n / 2);
            im.resize ((size_t) n / 2);
        }

        ~MagnitudeFft() { vDSP_destroy_fftsetup (setup); }
        MagnitudeFft (const MagnitudeFft&) = delete;
        MagnitudeFft& operator= (const MagnitudeFft&) = delete;

        int size() const noexcept { return n; }
        int numBins() const noexcept { return n / 2; }

        // `frame` must hold size() samples (oldest first). `mags` receives numBins() values.
        void process (const float* frame, float* mags) noexcept
        {
            vDSP_vmul (frame, 1, window.data(), 1, work.data(), 1, (vDSP_Length) n);
            DSPSplitComplex split { re.data(), im.data() };
            vDSP_ctoz (reinterpret_cast<const DSPComplex*> (work.data()), 2, &split, 1, (vDSP_Length) n / 2);
            vDSP_fft_zrip (setup, &split, 1, log2n, FFT_FORWARD);
            // zrip output is 2x the DFT; a Hann-windowed sine peaks at A*N/4.
            const float scale = 1.0f / (2.0f * (float) n / 4.0f);
            mags[0] = 0.0f; // DC (and packed Nyquist) are of no interest
            for (int k = 1; k < n / 2; ++k)
                mags[k] = std::sqrt (re[(size_t) k] * re[(size_t) k] + im[(size_t) k] * im[(size_t) k]) * scale;
        }

    private:
        int n;
        vDSP_Length log2n;
        FFTSetup setup;
        std::vector<float> window, work, re, im;
    };

    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1 = 0, z2 = 0;

        static Biquad lowPass (double sr, double fc, double q)
        {
            const double w = 2.0 * pi * fc / sr, c = std::cos (w), alpha = std::sin (w) / (2.0 * q);
            const double a0 = 1.0 + alpha;
            Biquad f;
            f.b0 = (1.0 - c) / 2.0 / a0; f.b1 = (1.0 - c) / a0; f.b2 = f.b0;
            f.a1 = -2.0 * c / a0;        f.a2 = (1.0 - alpha) / a0;
            return f;
        }

        static Biquad highPass (double sr, double fc, double q)
        {
            const double w = 2.0 * pi * fc / sr, c = std::cos (w), alpha = std::sin (w) / (2.0 * q);
            const double a0 = 1.0 + alpha;
            Biquad f;
            f.b0 = (1.0 + c) / 2.0 / a0; f.b1 = -(1.0 + c) / a0; f.b2 = f.b0;
            f.a1 = -2.0 * c / a0;        f.a2 = (1.0 - alpha) / a0;
            return f;
        }

        inline float process (float x) noexcept
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return (float) y;
        }

        void reset() noexcept { z1 = z2 = 0; }
    };

    // Integer-factor decimator to roughly 22 kHz with a 4th-order Butterworth
    // anti-alias filter and a rumble high-pass.
    class Decimator
    {
    public:
        void prepare (double hostRate)
        {
            factor = std::max (1, (int) std::lround (hostRate / 22050.0));
            outRate = hostRate / factor;
            const double fc = std::min (5000.0, 0.4 * outRate);
            lp1 = Biquad::lowPass (hostRate, fc, 0.54119610);
            lp2 = Biquad::lowPass (hostRate, fc, 1.30656296);
            hp  = Biquad::highPass (hostRate, 55.0, 0.70710678);
            phase = 0;
        }

        int getFactor() const noexcept { return factor; }
        double getOutputRate() const noexcept { return outRate; }

        // Returns true (and sets `out`) when a decimated sample is produced.
        inline bool push (float x, float& out) noexcept
        {
            const float y = lp2.process (lp1.process (hp.process (x)));
            if (++phase >= factor)
            {
                phase = 0;
                out = y;
                return true;
            }
            return false;
        }

        void reset() noexcept { lp1.reset(); lp2.reset(); hp.reset(); phase = 0; }

    private:
        int factor = 2, phase = 0;
        double outRate = 22050.0;
        Biquad lp1, lp2, hp;
    };
}
