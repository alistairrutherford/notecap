#pragma once

#include <JuceHeader.h>
#include "core/ChordEngine.h"
#include "core/Quantizer.h"
#include "core/Voicing.h"
#include <array>
#include <atomic>

// NoteCap: listens to its main (or sidechain) input, recognises chords and emits
// them as quantised MIDI on the plugin's MIDI output. In Live, a MIDI track
// records it with "MIDI From: <NoteCap's track> / NoteCap" (sample-accurate).
class NoteCapAudioProcessor : public juce::AudioProcessor
{
public:
    NoteCapAudioProcessor();
    ~NoteCapAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void reset() override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    // Lock-free read-outs for the editor.
    struct Readout
    {
        std::atomic<int32_t> chord { -1 }, previousChord { -1 }, sentChord { -1 };
        std::atomic<float> confidence { 0 }, levelDb { -100 }, tuningCents { 0 }, latencyMs { 0 }, bpm { 120 };
        std::array<std::atomic<float>, 12> chroma {};
        std::atomic<int> bassPc { -1 };
        std::atomic<bool> playing { false }, sidechainConnected { false }, lastWasLate { false };
        std::atomic<uint32_t> chordsSent { 0 };
    };
    Readout readout;

private:
    struct Pending
    {
        uint32_t segment = 0;
        int64_t fireAt = 0;    // sample at which the MIDI is emitted
        int64_t gridAt = 0;    // grid position it belongs to (earlier than fireAt when late)
        notecap::Chord chord;  // none = release
        uint8_t velocity = 100;
        bool releaseOnly = false;
        bool late = false;
        bool replacement = false;  // correction of the chord already sounding: send only the difference
    };

    struct Transport
    {
        bool playing = false;
        double bpm = 120.0, ppq = 0.0;
    };

    void updateEngineParams();
    void handleChordEvent (const notecap::ChordEvent& e, const Transport& t, int64_t blockStart);
    void addPending (const Pending& p);
    void scheduleTestChords (const Transport& t, int64_t blockStart, int numSamples);
    void firePending (juce::MidiBuffer& midi, const Transport& t, int64_t blockStart, int numSamples);
    void fire (const Pending& p, juce::MidiBuffer& midi, const Transport& t, int64_t blockStart, int numSamples);
    void allNotesOff (juce::MidiBuffer& midi, int sampleOffset);
    notecap::QuantizeGrid currentGrid() const;
    double stepPpq() const;

    // parameters
    std::atomic<float>* pSource = nullptr, *pGate = nullptr, *pSensitivity = nullptr, *pMinChord = nullptr,
                      * pRetrigger = nullptr, *pSevenths = nullptr, *pSuspended = nullptr, *pDimAug = nullptr,
                      * pGrid = nullptr, *pQMode = nullptr, *pSwing = nullptr, *pLength = nullptr,
                      * pVoicing = nullptr, *pOctave = nullptr, *pVelMode = nullptr, *pVelocity = nullptr,
                      * pChannel = nullptr, *pTestMode = nullptr;

    notecap::ChordEngine engine;
    notecap::ChordEventList events;

    juce::AudioBuffer<float> monoBuffer;
    double sampleRate = 44100.0;
    int64_t sampleCounter = 0;
    bool wasPlaying = false, bypassFlushed = false;
    // Set when the host resets or releases the plugin without a MIDI buffer to
    // send note-offs into; the next block ends whatever is still sounding.
    std::atomic<bool> flushRequested { false };

    static constexpr int maxPending = 64;
    std::array<Pending, maxPending> pending {};
    int numPending = 0;

    notecap::Voicing sounding;
    int soundingChannel = 1;
    uint32_t soundingSegment = 0;
    int32_t lastDetectedPacked = -1;
    double lastTestLinePpq = -1.0e9;
    uint32_t lastFiredSegment = 0;
    int64_t lastFiredGridAt = 0;
    notecap::Chord lastFiredChord;
    uint8_t lastFiredVelocity = 100;
    uint32_t testSegment = 0x80000000u;
    float latencyAvgMs = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NoteCapAudioProcessor)
};
