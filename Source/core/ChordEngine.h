// Real-time chord recognition: decimate -> frame -> onset + chroma -> classify -> track.
//
// All time values are in *host* samples (the sample rate passed to prepare()),
// counted from an arbitrary origin supplied by the caller for each block.
// No allocation after prepare(); safe to call from the audio thread.
#pragma once

#include "ChordClassifier.h"
#include "ChromaExtractor.h"
#include "Dsp.h"
#include "OnsetDetector.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace notecap
{
    struct ChordEvent
    {
        uint32_t segment = 0;      // events with the same segment refer to the same strum
        Chord chord;               // isNone() == chord released / silence
        int64_t onsetSample = 0;   // estimated start of the chord (in the past)
        int64_t detectSample = 0;  // when the decision was made
        float confidence = 0.0f;
        float levelDb = -100.0f;   // peak input level of the segment so far
        bool correction = false;   // replaces an earlier decision for the same segment
    };

    struct ChordEventList
    {
        static constexpr int capacity = 32;
        std::array<ChordEvent, capacity> items;
        int count = 0;

        void clear() noexcept { count = 0; }
        void add (const ChordEvent& e) noexcept { if (count < capacity) items[(size_t) count++] = e; }
        const ChordEvent* begin() const noexcept { return items.data(); }
        const ChordEvent* end() const noexcept { return items.data() + count; }
    };

    class ChordEngine
    {
    public:
        struct Params
        {
            float gateDb = -50.0f;       // input below this is treated as silence
            float minScore = 0.60f;      // classification confidence needed to emit
            float minChordMs = 150.0f;   // how long a change without an onset must persist
            float releaseMs = 200.0f;    // silence needed before a chord is released
            bool retrigger = false;      // re-emit when the same chord is strummed again
            bool sevenths = true, suspended = false, dimAug = false;
        };

        static constexpr int hopSize = 256;          // decimated samples (~11.6 ms at 22.05 kHz)
        static constexpr int minFramesToDecide = 2;
        static constexpr int maxFramesToDecide = 8;
        static constexpr float correctionMargin = 0.02f;
        static constexpr float baselineScale = 1.0f;   // previous chord removal strength
        static constexpr float decisionMargin = 0.03f;
        static constexpr double settleFraction = 0.55; // share of the chroma window the new chord must fill
        static constexpr double correctUntilFraction = 1.3;
        static constexpr int onsetLagSamples = 384;    // decimated; calibrated in test_engine

        void prepare (double hostSampleRate, int chromaFftSize = 4096)
        {
            hostRate = hostSampleRate;
            decimator.prepare (hostRate);
            factor = decimator.getFactor();
            const double fsA = decimator.getOutputRate();

            fftN = chromaFftSize;
            ringSize = 1;
            while (ringSize < fftN * 2) ringSize <<= 1;
            ring.assign ((size_t) ringSize, 0.0f);
            frame.assign ((size_t) fftN, 0.0f);

            chroma = std::make_unique<ChromaExtractor> (fsA, fftN);
            onset = std::make_unique<OnsetDetector> (fsA);
            onset->setMinGapFrames ((int) std::ceil (0.1 * fsA / hopSize));

            hopMs = 1000.0 * hopSize / fsA;
            setParams (params);
            reset();
        }

        void setParams (const Params& p) noexcept
        {
            params = p;
            classifier.setGroupEnabled (QualityGroup::Sevenths, p.sevenths);
            classifier.setGroupEnabled (QualityGroup::Suspended, p.suspended);
            classifier.setGroupEnabled (QualityGroup::DimAug, p.dimAug);
        }

        const Params& getParams() const noexcept { return params; }

        void reset() noexcept
        {
            std::fill (ring.begin(), ring.end(), 0.0f);
            writePos = 0; hopCount = 0;
            decimator.reset();
            if (onset) onset->reset();
            if (chroma) chroma->resetTuning();
            current = {}; currentScore = 0;
            segActive = segEmitted = segSettled = segWasEmittedAsEvent = false;
            silenceFrames = 0; divergeFrames = 0;
            ema = {}; displayChroma = {};
            noteHistory = {}; historyPos = 0; segBaseline = {};
            levelDb = -100.0f;
        }

        // Feed one block of mono audio. `blockStart` is the host sample index of x[0].
        void process (const float* x, int n, int64_t blockStart, ChordEventList& out) noexcept
        {
            for (int i = 0; i < n; ++i)
            {
                // A NaN/Inf from upstream would latch in the IIR filters forever.
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

        // --- read-outs for the UI (call from the audio thread and publish) ---
        const Chroma& getDisplayChroma() const noexcept { return displayChroma; }
        int getBassPitchClass() const noexcept { return displayBass; }
        double getTuningCents() const noexcept { return chroma ? chroma->getTuning() * 100.0 : 0.0; }
        float getLevelDb() const noexcept { return levelDb; }
        const Chord& getCurrentChord() const noexcept { return current; }
        float getCurrentScore() const noexcept { return currentScore; }
        int getDecimationFactor() const noexcept { return factor; }

    private:
        void analyseFrame (int64_t tHost, ChordEventList& out) noexcept
        {
            // Unroll the ring into a contiguous frame (oldest first).
            const int64_t start = writePos - fftN;
            for (int i = 0; i < fftN; ++i)
                frame[(size_t) i] = ring[(size_t) ((start + i) & (ringSize - 1))];

            const float* shortFrame = frame.data() + (fftN - OnsetDetector::fftSize);
            float sumSq = 0.0f;
            for (int i = 0; i < OnsetDetector::fftSize; ++i) sumSq += shortFrame[i] * shortFrame[i];
            levelDb = 10.0f * std::log10 (sumSq / OnsetDetector::fftSize + 1e-12f);

            const bool voiced = levelDb > params.gateDb;
            const bool isOnset = onset->process (shortFrame, voiced);
            const auto cr = chroma->process (frame.data(), voiced);
            noteHistory[(size_t) (historyPos++ % noteHistory.size())] = chroma->getNoteActivations();
            const Chroma frameChroma = normalised (cr.chroma);

            // Bass scaled relative to the whole chroma so quiet bass stays quiet.
            float cNorm = 0.0f;
            for (float v : cr.chroma) cNorm += v * v;
            cNorm = cNorm > 0 ? 1.0f / std::sqrt (cNorm) : 0.0f;
            Chroma frameBass {};
            for (size_t i = 0; i < 12; ++i) frameBass[i] = cr.bass[i] * cNorm;

            for (size_t i = 0; i < 12; ++i)
            {
                displayChroma[i] = 0.8f * displayChroma[i] + 0.2f * (voiced ? frameChroma[i] : 0.0f);
                ema[i] = 0.7f * ema[i] + 0.3f * frameChroma[i];
                emaBass[i] = 0.7f * emaBass[i] + 0.3f * frameBass[i];
            }
            displayBass = 0;
            for (int i = 1; i < 12; ++i)
                if (frameBass[(size_t) i] > frameBass[(size_t) displayBass]) displayBass = i;
            if (frameBass[(size_t) displayBass] <= 0.0f) displayBass = -1;

            const int64_t hopHost = (int64_t) hopSize * factor;
            const int64_t windowHost = (int64_t) fftN * factor;
            lastFrameTime = tHost;

            // ---- silence / release ----
            if (voiced)
                silenceFrames = 0;
            else
            {
                if (silenceFrames == 0) silenceStart = tHost - hopHost;
                ++silenceFrames;
                if (silenceFrames * hopMs >= params.releaseMs && (segActive || ! current.isNone()))
                {
                    segActive = false;
                    divergeFrames = 0;
                    if (! current.isNone())
                    {
                        current = {};
                        currentScore = 0.0f;
                        emit (out, ++segmentCounter, current, silenceStart, tHost, 0.0f, levelDb, false);
                    }
                }
            }

            // ---- segment start ----
            if (isOnset && voiced)
                startSegment (tHost - hopHost - (int64_t) onsetLagSamples * factor, levelDb);
            else if (voiced && ! segActive && current.isNone())
                startSegment (tHost - hopHost, levelDb);

            // ---- segment classification ----
            if (segActive && voiced)
            {
                segPeakDb = std::max (segPeakDb, levelDb);
                const double frac = (double) (tHost - segStart) / (double) windowHost;

                if (frac >= settleFraction && ! segSettled)
                {
                    // The start of the window still holds the previous chord: remove the
                    // notes that were sounding before the onset, in proportion to the
                    // (Hann-weighted) share of the window that precedes it.
                    const double old = std::max (0.0, 1.0 - frac);
                    const float share = (float) (old - std::sin (2.0 * pi * old) / (2.0 * pi));
                    const auto sc = chroma->foldWithout (segBaseline, baselineScale * share);
                    const Chroma segFrameChroma = normalised (sc.chroma);
                    float sn = 0.0f;
                    for (float v : sc.chroma) sn += v * v;
                    sn = sn > 0 ? 1.0f / std::sqrt (sn) : 0.0f;
                    for (size_t i = 0; i < 12; ++i) { segAcc[i] += segFrameChroma[i]; segBass[i] += sc.bass[i] * sn; }
                    ++segFrames;

                    if (segFrames >= minFramesToDecide)
                    {
                        const auto cls = classifier.classify (segAcc, segBass);
                        // Commit early only when the winner is clear; a close call
                        // (Am vs Am7) waits for more of the window to fill.
                        const bool clearWinner = cls.score - cls.runnerUp >= decisionMargin
                                                 || segFrames >= maxFramesToDecide;
                        if (! segEmitted)
                        {
                            if (cls.score >= params.minScore && clearWinner)
                            {
                                segEmitted = true;
                                segChord = cls.chord;
                                if (current.isNone() || ! cls.chord.sameHarmony (current) || params.retrigger)
                                {
                                    segWasEmittedAsEvent = true;
                                    current = cls.chord;
                                    currentScore = cls.score;
                                    emit (out, segmentId, cls.chord, segStart, tHost, cls.score, segPeakDb, false);
                                }
                            }
                        }
                        else if (cls.score >= params.minScore && ! cls.chord.sameHarmony (segChord)
                                 && cls.score - classifier.score (segAcc, segChord) >= correctionMargin)
                        {
                            // More of the window now belongs to this strum: revise the decision.
                            segChord = cls.chord;
                            current = cls.chord;
                            currentScore = cls.score;
                            emit (out, segmentId, cls.chord, segStart, tHost, cls.score, segPeakDb, segWasEmittedAsEvent);
                            segWasEmittedAsEvent = true;
                        }
                    }
                }

                if (frac > correctUntilFraction)
                    segSettled = true;
            }

            // ---- legato changes (no onset) once the segment has settled ----
            if (voiced && ! current.isNone() && (segSettled || ! segActive))
            {
                const auto cls = classifier.classify (ema, emaBass);
                const float currentFit = classifier.score (ema, current);
                // Same-root changes (Em -> Em7) during a sustain are usually partials
                // decaying at different rates, not a new chord, so they need an onset.
                if (cls.score >= params.minScore + 0.03f && cls.score > currentFit + 0.05f
                    && cls.chord.root != current.root)
                {
                    if (divergeFrames == 0 || ! cls.chord.sameHarmony (divergeChord))
                    {
                        divergeFrames = 0;
                        divergeChord = cls.chord;
                        divergeStart = tHost - windowHost / 2;
                    }
                    if (++divergeFrames * hopMs >= params.minChordMs)
                    {
                        startSegment (divergeStart, levelDb);
                        segEmitted = segSettled = segWasEmittedAsEvent = true;
                        segChord = current = cls.chord;
                        currentScore = cls.score;
                        emit (out, segmentId, cls.chord, divergeStart, tHost, cls.score, segPeakDb, false);
                    }
                }
                else
                    divergeFrames = 0;
            }
        }

        void startSegment (int64_t at, float db) noexcept
        {
            // Baseline = activations from the last frame that ended before the onset.
            const int framesBack = (int) std::min<int64_t> ((int64_t) noteHistory.size() - 1,
                                                            std::max<int64_t> (1, (lastFrameTime - at) / ((int64_t) hopSize * factor) + 1));
            segBaseline = noteHistory[(size_t) ((historyPos - 1 - framesBack + (int) noteHistory.size() * 4) % (int) noteHistory.size())];
            segActive = true;
            segmentId = ++segmentCounter;
            segStart = at;
            segAcc = {}; segBass = {};
            segFrames = 0;
            segEmitted = segSettled = segWasEmittedAsEvent = false;
            segPeakDb = db;
            divergeFrames = 0;
        }

        static void emit (ChordEventList& out, uint32_t seg, const Chord& c, int64_t onsetAt,
                          int64_t detectAt, float conf, float db, bool correction) noexcept
        {
            ChordEvent e;
            e.segment = seg; e.chord = c; e.onsetSample = onsetAt; e.detectSample = detectAt;
            e.confidence = conf; e.levelDb = db; e.correction = correction;
            out.add (e);
        }

        Params params;
        double hostRate = 44100.0, hopMs = 11.6;
        Decimator decimator;
        int factor = 2, fftN = 4096, ringSize = 8192, hopCount = 0;
        int64_t writePos = 0;
        std::vector<float> ring, frame;
        std::unique_ptr<ChromaExtractor> chroma;
        std::unique_ptr<OnsetDetector> onset;
        ChordClassifier classifier;

        Chord current; float currentScore = 0;
        uint32_t segmentCounter = 0, segmentId = 0;
        bool segActive = false, segEmitted = false, segSettled = false, segWasEmittedAsEvent = false;
        int64_t segStart = 0; int segFrames = 0; float segPeakDb = -100;
        Chroma segAcc {}, segBass {};
        Chord segChord;
        ChromaExtractor::Notes segBaseline {};
        std::array<ChromaExtractor::Notes, 8> noteHistory {};
        int historyPos = 0;
        int64_t lastFrameTime = 0;

        int silenceFrames = 0; int64_t silenceStart = 0;
        int divergeFrames = 0; int64_t divergeStart = 0; Chord divergeChord;
        Chroma ema {}, emaBass {}, displayChroma {};
        int displayBass = -1;
        float levelDb = -100.0f;
    };
}
