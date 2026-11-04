// Real-time single-note (monophonic) tracking: decimate -> YIN pitch + onsets -> notes.
//
// For single-note lines (a guitar melody, a bass line, a voice). Same conventions as
// ChordEngine: times are host samples from the caller's origin; no allocation after
// prepare(); safe on the audio thread.
#pragma once

#include "ChordTypes.h"
#include "Dsp.h"
#include "OnsetDetector.h"
#include <Accelerate/Accelerate.h>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace notecap
{
    // YIN fundamental-frequency estimator (de Cheveigné & Kawahara, 2002), on the decimated signal.
    class YinPitch
    {
    public:
        static constexpr double minHz = 70.0;     // below a guitar's low E (82 Hz), with room for drop tunings
        static constexpr double maxHz = 1500.0;   // above a guitar's 24th fret on the high E (1319 Hz)
        static constexpr int window = 1024;       // integration window (decimated samples, ~46 ms)
        static constexpr float threshold = 0.15f; // first dip of the normalised difference below this

        void prepare (double sampleRate)
        {
            fs = sampleRate;
            tauMin = std::max (2, (int) std::floor (fs / maxHz));
            tauMax = (int) std::ceil (fs / minHz);
            diff.assign ((size_t) tauMax + 2, 0.0f);
            energy.assign ((size_t) (window + tauMax + 1), 0.0f);
        }

        int frameLength() const noexcept { return window + tauMax + 1; }

        struct Result { double hz = 0.0; float aperiodicity = 1.0f; };

        // `x` holds frameLength() samples, oldest first.
        Result process (const float* x) noexcept
        {
            Result r;
            const int len = frameLength();
            energy[0] = 0.0f;
            for (int i = 0; i < len - 1; ++i) energy[(size_t) i + 1] = energy[(size_t) i] + x[i] * x[i];

            const float e0 = energy[(size_t) window] - energy[0];
            if (e0 < 1.0e-9f)
                return r;

            // d(tau) = sum (x_j - x_{j+tau})^2 = E(0..W) + E(tau..tau+W) - 2 acf(tau); then the
            // cumulative-mean-normalised difference d'(tau).
            float running = 0.0f;
            diff[0] = 1.0f;
            for (int tau = 1; tau <= tauMax; ++tau)
            {
                float acf = 0.0f;
                vDSP_dotpr (x, 1, x + tau, 1, &acf, (vDSP_Length) window);
                const float et = energy[(size_t) (tau + window)] - energy[(size_t) tau];
                const float d = std::max (0.0f, e0 + et - 2.0f * acf);
                running += d;
                diff[(size_t) tau] = running > 0.0f ? d * (float) tau / running : 1.0f;
            }

            int best = -1;
            for (int tau = tauMin; tau <= tauMax; ++tau)
                if (diff[(size_t) tau] < threshold)
                {
                    while (tau + 1 <= tauMax && diff[(size_t) tau + 1] < diff[(size_t) tau]) ++tau;
                    best = tau;
                    break;
                }
            if (best < 0)
            {
                best = tauMin;   // nothing clearly periodic: report the deepest dip as aperiodic
                for (int tau = tauMin + 1; tau <= tauMax; ++tau)
                    if (diff[(size_t) tau] < diff[(size_t) best]) best = tau;
            }

            double t = best;
            if (best > tauMin && best < tauMax)
            {
                const double a = diff[(size_t) best - 1], b = diff[(size_t) best], c = diff[(size_t) best + 1];
                const double den = a - 2.0 * b + c;
                if (std::abs (den) > 1.0e-12) t += 0.5 * (a - c) / den;
            }
            r.hz = fs / t;
            r.aperiodicity = diff[(size_t) best];
            return r;
        }

    private:
        double fs = 22050.0;
        int tauMin = 14, tauMax = 315;
        std::vector<float> diff, energy;
    };

    struct NoteEvent
    {
        uint32_t segment = 0;
        int note = -1;             // MIDI note number; -1 = the note ended
        int64_t onsetSample = 0;   // when it started (in the past)
        int64_t detectSample = 0;  // when it was decided
        float levelDb = -100.0f;
        float confidence = 0.0f;   // 1 - YIN aperiodicity
    };

    struct NoteEventList
    {
        static constexpr int capacity = 32;
        std::array<NoteEvent, capacity> items;
        int count = 0;

        void clear() noexcept { count = 0; }
        void add (const NoteEvent& e) noexcept { if (count < capacity) items[(size_t) count++] = e; }
        const NoteEvent* begin() const noexcept { return items.data(); }
        const NoteEvent* end() const noexcept { return items.data() + count; }
    };

    class NoteTracker
    {
    public:
        struct Params
        {
            float gateDb = -50.0f;     // input below this is silence
            float minNoteMs = 30.0f;   // a new pitch must hold this long to become a note
            float releaseMs = 60.0f;   // silence (or no clear pitch) this long ends the note
            bool retrigger = true;     // a re-picked note (same pitch) starts a new note
        };

        static constexpr int hopSize = 256;            // decimated samples (~11.6 ms at 22.05 kHz)
        static constexpr float maxAperiodicity = 0.25f; // above this a frame has no usable pitch
        static constexpr double holdSemitones = 0.6;   // vibrato / slight bends stay on the note
        static constexpr int onsetLagSamples = 384;    // decimated; as ChordEngine

        void prepare (double hostSampleRate)
        {
            decimator.prepare (hostSampleRate);
            factor = decimator.getFactor();
            const double fsA = decimator.getOutputRate();
            yin.prepare (fsA);
            ringSize = 1;
            while (ringSize < std::max (yin.frameLength(), OnsetDetector::fftSize) * 2) ringSize <<= 1;
            ring.assign ((size_t) ringSize, 0.0f);
            frame.assign ((size_t) std::max (yin.frameLength(), OnsetDetector::fftSize), 0.0f);
            onset = std::make_unique<OnsetDetector> (fsA);
            onset->setMinGapFrames ((int) std::ceil (0.05 * fsA / hopSize));
            hopMs = 1000.0 * hopSize / fsA;
            reset();
        }

        void setParams (const Params& p) noexcept { params = p; }
        const Params& getParams() const noexcept { return params; }

        void reset() noexcept
        {
            std::fill (ring.begin(), ring.end(), 0.0f);
            writePos = 0; hopCount = 0;
            decimator.reset();
            if (onset) onset->reset();
            current = candidate = -1;
            candidateFrames = silentFrames = 0;
            onsetArmed = false;
            tuning = 0.0; tuningPhasor = { 1.0, 0.0 };
            levelDb = -100.0f; confidence = 0.0f; pitchMidi = 0.0;
        }

        void process (const float* x, int n, int64_t blockStart, NoteEventList& out) noexcept
        {
            for (int i = 0; i < n; ++i)
            {
                const float in = std::isfinite (x[i]) ? x[i] : 0.0f;
                float y;
                if (! decimator.push (in, y)) continue;
                ring[(size_t) (writePos++ & (ringSize - 1))] = y;
                if (++hopCount >= hopSize)
                {
                    hopCount = 0;
                    analyseFrame (blockStart + i + 1, out);
                }
            }
        }

        // --- read-outs ---
        int getCurrentNote() const noexcept { return current; }
        float getConfidence() const noexcept { return confidence; }
        float getLevelDb() const noexcept { return levelDb; }
        double getTuningCents() const noexcept { return tuning * 100.0; }
        double getPitchMidi() const noexcept { return pitchMidi; }   // latest fractional pitch (0 when none)

    private:
        void analyseFrame (int64_t tHost, NoteEventList& out) noexcept
        {
            const int len = (int) frame.size();
            const int64_t start = writePos - len;
            for (int i = 0; i < len; ++i)
                frame[(size_t) i] = ring[(size_t) ((start + i) & (ringSize - 1))];

            const float* shortFrame = frame.data() + (len - OnsetDetector::fftSize);
            float sumSq = 0.0f;
            for (int i = 0; i < OnsetDetector::fftSize; ++i) sumSq += shortFrame[i] * shortFrame[i];
            levelDb = 10.0f * std::log10 (sumSq / OnsetDetector::fftSize + 1e-12f);
            const bool voiced = levelDb > params.gateDb;

            const int64_t hopHost = (int64_t) hopSize * factor;
            if (onset->process (shortFrame, voiced) && voiced)
            {
                onsetArmed = true;
                onsetTime = tHost - hopHost - (int64_t) onsetLagSamples * factor;
            }

            const auto p = yin.process (frame.data() + (len - yin.frameLength()));
            const bool pitched = voiced && p.hz > 0.0 && p.aperiodicity < maxAperiodicity;
            confidence = pitched ? 1.0f - p.aperiodicity : 0.0f;
            const int minFrames = std::max (1, (int) std::lround (params.minNoteMs / hopMs));

            if (! pitched)
            {
                pitchMidi = 0.0;
                if (silentFrames++ == 0) silenceStart = tHost - hopHost;
                candidate = -1;
                candidateFrames = 0;
                if (current >= 0 && silentFrames * hopMs >= params.releaseMs)
                {
                    emit (out, -1, silenceStart, tHost);
                    current = -1;
                }
                return;
            }
            silentFrames = 0;

            // Just after a pluck the YIN window still holds mostly the previous note: wait until
            // about half of it is the new attack before deciding anything.
            if (onsetArmed && tHost - onsetTime < (int64_t) (yin.frameLength() / 2) * factor)
                return;

            const double raw = 69.0 + 12.0 * std::log2 (p.hz / 440.0);
            updateTuning (raw, confidence);
            pitchMidi = raw - tuning;
            const int nearest = (int) std::lround (pitchMidi);

            // Still on the current note (vibrato, slight bend) and not re-picked: nothing to do.
            const bool sameAsCurrent = current >= 0 && std::abs (pitchMidi - current) < holdSemitones;
            if (sameAsCurrent && ! (onsetArmed && params.retrigger))
            {
                onsetArmed = false;
                candidate = -1;
                candidateFrames = 0;
                return;
            }

            const int target = sameAsCurrent ? current : nearest;
            if (target != candidate)
            {
                candidate = target;
                candidateFrames = 0;
                // A plucked note starts at its onset; a legato change (hammer-on, slide) where
                // the pitch moved: about half a YIN window before this frame.
                candidateStart = onsetArmed ? onsetTime
                                            : tHost - (int64_t) (yin.frameLength() / 2) * factor;
            }
            if (++candidateFrames < minFrames)
                return;

            if (current >= 0)
                emit (out, -1, candidateStart, tHost);
            current = candidate;
            emit (out, current, candidateStart, tHost);
            candidate = -1;
            candidateFrames = 0;
            onsetArmed = false;
        }

        void updateTuning (double rawMidi, float weight) noexcept
        {
            const double dev = rawMidi - std::round (rawMidi);
            tuningPhasor = 0.99 * tuningPhasor + 0.01 * (double) weight * std::polar (1.0, 2.0 * pi * dev);
            tuning = std::arg (tuningPhasor) / (2.0 * pi);
        }

        void emit (NoteEventList& out, int note, int64_t onsetAt, int64_t detectAt) noexcept
        {
            NoteEvent e;
            e.segment = note >= 0 ? ++segmentCounter : segmentCounter;
            e.note = note;
            e.onsetSample = onsetAt;
            e.detectSample = detectAt;
            e.levelDb = levelDb;
            e.confidence = confidence;
            out.add (e);
        }

        Params params;
        Decimator decimator;
        YinPitch yin;
        std::unique_ptr<OnsetDetector> onset;
        std::vector<float> ring, frame;
        int factor = 2, ringSize = 4096, hopCount = 0;
        int64_t writePos = 0;
        double hopMs = 11.6;

        int current = -1, candidate = -1, candidateFrames = 0, silentFrames = 0;
        int64_t candidateStart = 0, silenceStart = 0, onsetTime = 0;
        bool onsetArmed = false;
        uint32_t segmentCounter = 0;
        double tuning = 0.0, pitchMidi = 0.0;
        std::complex<double> tuningPhasor { 1.0, 0.0 };
        float levelDb = -100.0f, confidence = 0.0f;
    };

    // Ableton's octave naming: middle C (MIDI 60) is C3.
    inline std::string noteName (int midi)
    {
        if (midi < 0) return "-";
        return std::string (pitchClassName (midi % 12)) + std::to_string (midi / 12 - 2);
    }
}
