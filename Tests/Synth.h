// Test-signal generators: Karplus-Strong strummed guitar chords and additive tones.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace testsynth
{
    struct KsString
    {
        std::vector<float> buf;
        size_t pos = 0;
        float decay = 0.996f, last = 0.0f;
        bool active = false;

        void pluck (double freq, double fs, float amp, std::mt19937& rng)
        {
            const int n = std::max (2, (int) std::lround (fs / freq - 0.5));
            buf.assign ((size_t) n, 0.0f);
            std::uniform_real_distribution<float> d (-1.0f, 1.0f);
            float lp = 0.0f;
            for (auto& s : buf) { lp = 0.5f * lp + 0.5f * d (rng); s = amp * lp; }
            pos = 0; last = 0; decay = 0.996f; active = true;
        }

        float tick()
        {
            if (! active) return 0.0f;
            const size_t next = (pos + 1) % buf.size();
            const float out = buf[pos];
            buf[pos] = decay * 0.5f * (buf[pos] + buf[next]);
            pos = next;
            return out;
        }
    };

    inline double midiToHz (double n) { return 440.0 * std::pow (2.0, (n - 69.0) / 12.0); }

    struct Strum
    {
        double timeSec;
        std::vector<int> notes;  // MIDI, low to high
        std::string label;       // expected chord name (harmony only)
    };

    // Renders strums on a 6-voice guitar; each strum damps the previous one.
    inline std::vector<float> renderGuitar (const std::vector<Strum>& strums, double fs, double lengthSec,
                                            double detuneCents = 0.0, unsigned seed = 1,
                                            double strumSpreadMs = 12.0)
    {
        std::mt19937 rng (seed);
        std::uniform_real_distribution<float> ampRand (0.7f, 1.0f);
        std::normal_distribution<float> noise (0.0f, 0.0003f);
        std::vector<float> out ((size_t) (lengthSec * fs), 0.0f);
        std::vector<KsString> strings (6);

        struct Pending { int64_t at; int string; double freq; float amp; };
        std::vector<Pending> plucks;
        std::vector<int64_t> damps;
        for (const auto& s : strums)
        {
            const int64_t t0 = (int64_t) (s.timeSec * fs);
            damps.push_back (t0);
            for (size_t i = 0; i < s.notes.size() && i < 6; ++i)
                plucks.push_back ({ t0 + (int64_t) (i * strumSpreadMs * 0.001 * fs), (int) i,
                                    midiToHz (s.notes[i] + detuneCents / 100.0), 0.25f * ampRand (rng) });
        }

        for (int64_t n = 0; n < (int64_t) out.size(); ++n)
        {
            for (auto d : damps)
                if (d == n) for (auto& st : strings) st.decay = 0.95f;
            for (const auto& p : plucks)
                if (p.at == n) strings[(size_t) p.string].pluck (p.freq, fs, p.amp, rng);
            float y = noise (rng);
            for (auto& st : strings) y += st.tick();
            out[(size_t) n] = y;
        }
        return out;
    }

    // Sustained additive tones (piano/organ-ish) with 8 decaying harmonics.
    inline void addTone (std::vector<float>& out, double fs, double startSec, double durSec,
                         double midiNote, float amp)
    {
        const double f = midiToHz (midiNote);
        const int64_t s0 = (int64_t) (startSec * fs), len = (int64_t) (durSec * fs);
        for (int64_t i = 0; i < len && s0 + i < (int64_t) out.size(); ++i)
        {
            const double t = (double) i / fs;
            const double env = std::min (1.0, t / 0.005) * std::exp (-t * 1.5) * std::min (1.0, (double) (len - i) / (0.01 * fs));
            double y = 0.0;
            for (int h = 1; h <= 8; ++h)
                if (f * h < fs * 0.45) y += std::sin (2.0 * M_PI * f * h * t) * std::pow (0.6, h - 1);
            out[(size_t) (s0 + i)] += (float) (amp * env * y);
        }
    }

    // Common guitar voicings (MIDI note numbers, low string first).
    inline std::vector<int> guitarChord (const std::string& name)
    {
        if (name == "C")     return { 48, 52, 55, 60, 64 };
        if (name == "Am")    return { 45, 52, 57, 60, 64 };
        if (name == "F")     return { 41, 48, 53, 57, 60, 65 };
        if (name == "G")     return { 43, 47, 50, 55, 59, 67 };
        if (name == "Em")    return { 40, 47, 52, 55, 59, 64 };
        if (name == "D")     return { 50, 57, 62, 66 };
        if (name == "Dm")    return { 50, 57, 62, 65 };
        if (name == "A")     return { 45, 52, 57, 61, 64 };
        if (name == "E")     return { 40, 47, 52, 56, 59, 64 };
        if (name == "E7")    return { 40, 47, 50, 56, 59, 64 };
        if (name == "Dm7")   return { 50, 57, 60, 65 };
        if (name == "Cmaj7") return { 48, 52, 55, 59, 64 };
        if (name == "Bm7")   return { 47, 50, 57, 59, 66 };
        if (name == "A7")    return { 45, 52, 55, 61, 64 };
        if (name == "Bb")    return { 46, 53, 58, 62, 65 };
        if (name == "Bm")    return { 47, 54, 59, 62, 66 };
        return {};
    }
}
