#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace notecap;

namespace
{
    const juce::StringArray gridNames { "Off", "1 Bar", "1/2", "1/4", "1/8", "1/16", "1/4T", "1/8T", "1/16T" };
    constexpr int lengthSteps[] = { 0, 1, 2, 4, 8 };
}

juce::AudioProcessorValueTreeState::ParameterLayout NoteCapAudioProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> ps;
    auto pct = AudioParameterFloatAttributes().withLabel ("%");
    auto ms  = AudioParameterFloatAttributes().withLabel ("ms");

    // Detection
    ps.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "source", 1 }, "Source", StringArray { "Sidechain", "Main input" }, 1));
    ps.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "gate", 1 }, "Gate", NormalisableRange<float> (-80.0f, -20.0f, 0.5f), -50.0f,
                                                         AudioParameterFloatAttributes().withLabel ("dB")));
    ps.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "sensitivity", 1 }, "Sensitivity", NormalisableRange<float> (0.0f, 100.0f, 1.0f), 50.0f, pct));
    ps.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "minChord", 1 }, "Min Change", NormalisableRange<float> (50.0f, 500.0f, 1.0f), 150.0f, ms));
    ps.push_back (std::make_unique<AudioParameterBool> (ParameterID { "retrigger", 1 }, "Retrigger", false));
    ps.push_back (std::make_unique<AudioParameterBool> (ParameterID { "sevenths", 1 }, "7th Chords", true));
    ps.push_back (std::make_unique<AudioParameterBool> (ParameterID { "suspended", 1 }, "Sus Chords", false));
    ps.push_back (std::make_unique<AudioParameterBool> (ParameterID { "dimAug", 1 }, "Dim/Aug Chords", false));

    // Quantise
    ps.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "grid", 1 }, "Grid", gridNames, 4));
    ps.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "qmode", 1 }, "Quantize Mode", StringArray { "Nearest", "Next line" }, 0));
    ps.push_back (std::make_unique<AudioParameterFloat> (ParameterID { "swing", 1 }, "Swing", NormalisableRange<float> (0.0f, 100.0f, 1.0f), 0.0f, pct));
    ps.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "length", 1 }, "Length", StringArray { "Hold", "1 step", "2 steps", "4 steps", "8 steps" }, 0));

    // Output
    ps.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "voicing", 1 }, "Voicing", StringArray { "Close", "Open", "Bass + chord" }, 0));
    ps.push_back (std::make_unique<AudioParameterInt> (ParameterID { "octave", 1 }, "Octave", 1, 5, 3));
    ps.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "velMode", 1 }, "Velocity Mode", StringArray { "From input", "Fixed" }, 0));
    ps.push_back (std::make_unique<AudioParameterInt> (ParameterID { "velocity", 1 }, "Velocity", 1, 127, 100));
    ps.push_back (std::make_unique<AudioParameterInt> (ParameterID { "channel", 1 }, "MIDI Channel", 1, 16, 1));
    ps.push_back (std::make_unique<AudioParameterBool> (ParameterID { "testMode", 1 }, "Test Chords", false));

    return { ps.begin(), ps.end() };
}

NoteCapAudioProcessor::NoteCapAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                          .withInput ("Sidechain", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "NoteCap", createLayout())
{
    auto get = [this] (const char* id) { return apvts.getRawParameterValue (id); };
    pSource = get ("source"); pGate = get ("gate"); pSensitivity = get ("sensitivity"); pMinChord = get ("minChord");
    pRetrigger = get ("retrigger"); pSevenths = get ("sevenths"); pSuspended = get ("suspended"); pDimAug = get ("dimAug");
    pGrid = get ("grid"); pQMode = get ("qmode"); pSwing = get ("swing"); pLength = get ("length");
    pVoicing = get ("voicing"); pOctave = get ("octave"); pVelMode = get ("velMode"); pVelocity = get ("velocity");
    pChannel = get ("channel"); pTestMode = get ("testMode");

    engine.prepare (sampleRate);
}

NoteCapAudioProcessor::~NoteCapAudioProcessor() = default;

void NoteCapAudioProcessor::updateEngineParams()
{
    ChordEngine::Params p;
    p.gateDb = pGate->load();
    p.minScore = 0.75f - 0.3f * pSensitivity->load() / 100.0f;
    p.minChordMs = pMinChord->load();
    p.retrigger = pRetrigger->load() > 0.5f;
    p.sevenths = pSevenths->load() > 0.5f;
    p.suspended = pSuspended->load() > 0.5f;
    p.dimAug = pDimAug->load() > 0.5f;
    engine.setParams (p);
}

QuantizeGrid NoteCapAudioProcessor::currentGrid() const
{
    QuantizeGrid g;
    g.stepPpq = gridStepForIndex ((int) pGrid->load());
    g.swing = pSwing->load() / 100.0;
    return g;
}

double NoteCapAudioProcessor::stepPpq() const
{
    const auto g = currentGrid();
    return g.active() ? g.stepPpq : 0.5;
}

//==============================================================================
void NoteCapAudioProcessor::prepareToPlay (double sr, int samplesPerBlock)
{
    if (sounding.count > 0) flushRequested = true;
    sampleRate = sr;
    engine.prepare (sr);
    updateEngineParams();
    monoBuffer.setSize (1, std::max (samplesPerBlock, 32));
    numPending = 0;
    wasPlaying = false;
    lastTestLinePpq = -1.0e9;
}

void NoteCapAudioProcessor::releaseResources()
{
    flushRequested = true;
}

void NoteCapAudioProcessor::reset()
{
    // JUCE's VST3 wrapper calls this from setProcessing (false), e.g. when Live
    // switches the device off. There's no MIDI buffer here, so defer the note-offs.
    flushRequested = true;
}

bool NoteCapAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    if (in != out || (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo()))
        return false;

    if (layouts.inputBuses.size() > 1)
    {
        const auto side = layouts.getChannelSet (true, 1);
        if (! side.isDisabled() && side != juce::AudioChannelSet::mono() && side != juce::AudioChannelSet::stereo())
            return false;
    }
    return true;
}

//==============================================================================
void NoteCapAudioProcessor::processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer& midi)
{
    if (! bypassFlushed)
    {
        numPending = 0;
        allNotesOff (midi, 0);
        bypassFlushed = true;
    }
}

void NoteCapAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    midi.clear();
    bypassFlushed = false;

    Transport t;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            t.playing = pos->getIsPlaying();
            if (auto b = pos->getBpm(); b && std::isfinite (*b)) t.bpm = juce::jlimit (20.0, 999.0, *b);
            if (auto p = pos->getPpqPosition(); p && std::isfinite (*p)) t.ppq = *p;
            else t.playing = false;   // no usable song position: don't quantise
        }

    updateEngineParams();

    // Stopping the transport ends any recording: release held chords, drop plans.
    // So does a host reset/release since the last block.
    if ((wasPlaying && ! t.playing) || flushRequested.exchange (false))
    {
        numPending = 0;
        allNotesOff (midi, 0);
        lastTestLinePpq = -1.0e9;
    }
    wasPlaying = t.playing;

    // ---- analysis input (mono) ----
    if (monoBuffer.getNumSamples() < n)
        monoBuffer.setSize (1, n, false, false, true);
    auto* mono = monoBuffer.getWritePointer (0);
    juce::FloatVectorOperations::clear (mono, n);

    const bool hasSidechain = getBusCount (true) > 1 && getBus (true, 1)->isEnabled()
                              && getBus (true, 1)->getNumberOfChannels() > 0;
    readout.sidechainConnected.store (hasSidechain);

    const bool useSidechain = pSource->load() < 0.5f;
    if (! useSidechain || hasSidechain)
    {
        const auto src = getBusBuffer (buffer, true, useSidechain ? 1 : 0);
        const int chans = src.getNumChannels();
        for (int c = 0; c < chans; ++c)
            juce::FloatVectorOperations::addWithMultiply (mono, src.getReadPointer (c), 1.0f / (float) chans, n);
    }

    // ---- recognise, schedule, emit ----
    events.clear();
    engine.process (mono, n, sampleCounter, events);

    if (pTestMode->load() > 0.5f)
        scheduleTestChords (t, sampleCounter, n);
    else
        for (const auto& e : events)
            handleChordEvent (e, t, sampleCounter);

    firePending (midi, t, sampleCounter, n);

    // ---- read-outs ----
    if (pTestMode->load() < 0.5f)
    {
        const int32_t packed = engine.getCurrentChord().pack();
        if (packed != lastDetectedPacked)
        {
            if (lastDetectedPacked >= 0) readout.previousChord.store (lastDetectedPacked);
            lastDetectedPacked = packed;
            readout.chord.store (packed);
        }
        readout.confidence.store (engine.getCurrentScore());
    }
    readout.levelDb.store (engine.getLevelDb());
    readout.tuningCents.store ((float) engine.getTuningCents());
    readout.latencyMs.store (latencyAvgMs);
    readout.bpm.store ((float) t.bpm);
    readout.playing.store (t.playing);
    readout.bassPc.store (engine.getBassPitchClass());
    const auto& ch = engine.getDisplayChroma();
    for (size_t i = 0; i < 12; ++i) readout.chroma[i].store (ch[i]);

    sampleCounter += n;
}

//==============================================================================
void NoteCapAudioProcessor::handleChordEvent (const ChordEvent& e, const Transport& t, int64_t blockStart)
{
    if (! e.chord.isNone() && ! e.correction)
        latencyAvgMs = latencyAvgMs <= 0.0f ? (float) ((e.detectSample - e.onsetSample) * 1000.0 / sampleRate)
                                            : 0.8f * latencyAvgMs + 0.2f * (float) ((e.detectSample - e.onsetSample) * 1000.0 / sampleRate);

    // Refine a chord that's still waiting for its grid line.
    for (int i = 0; i < numPending; ++i)
        if (pending[(size_t) i].segment == e.segment && ! pending[(size_t) i].releaseOnly)
        {
            pending[(size_t) i].chord = e.chord;
            return;
        }

    if (e.correction)
    {
        // Already sent: change it now.
        if (lastFiredSegment == e.segment && ! lastFiredChord.sameHarmony (e.chord))
        {
            Pending p;
            p.segment = e.segment; p.fireAt = e.detectSample; p.gridAt = lastFiredGridAt;
            p.chord = e.chord; p.late = true; p.replacement = true;
            p.velocity = lastFiredVelocity;
            addPending (p);
        }
        return;
    }

    const double spp = sampleRate * 60.0 / t.bpm;  // samples per quarter note
    auto toPpq = [&] (int64_t s) { return t.ppq + (double) (s - blockStart) / spp; };
    auto toSample = [&] (double ppq) { return blockStart + (int64_t) std::llround ((ppq - t.ppq) * spp); };

    Pending p;
    p.segment = e.segment;
    p.chord = e.chord;

    const auto grid = currentGrid();
    if (t.playing && grid.active())
    {
        const auto mode = pQMode->load() < 0.5f ? QuantizeMode::Nearest : QuantizeMode::Next;
        const auto d = quantize (toPpq (e.onsetSample), toPpq (e.detectSample), mode, grid);
        p.fireAt = std::max (toSample (d.fireAtPpq), e.detectSample);
        p.gridAt = std::min (toSample (d.gridPpq), p.fireAt);
        p.late = d.late;
    }
    else
        p.fireAt = p.gridAt = e.detectSample;

    if (pVelMode->load() < 0.5f)
    {
        const float gate = pGate->load();
        p.velocity = (uint8_t) juce::jlimit (1, 127, (int) std::lround (40.0f + 87.0f * juce::jlimit (0.0f, 1.0f, (e.levelDb - gate) / 40.0f)));
    }
    else
        p.velocity = (uint8_t) juce::jlimit (1, 127, (int) pVelocity->load());

    // A newer change supersedes any not-yet-sent change planned for the same time or later.
    for (int i = numPending; --i >= 0;)
        if (! pending[(size_t) i].releaseOnly && pending[(size_t) i].fireAt >= p.fireAt)
            pending[(size_t) i] = pending[(size_t) --numPending];

    addPending (p);
}

void NoteCapAudioProcessor::addPending (const Pending& p)
{
    if (numPending < maxPending)
        pending[(size_t) numPending++] = p;
}

void NoteCapAudioProcessor::scheduleTestChords (const Transport& t, int64_t blockStart, int numSamples)
{
    if (! t.playing)
        return;

    static const Chord cycle[] = { { 0, Quality::Major, -1 }, { 5, Quality::Major, -1 },
                                   { 7, Quality::Major, -1 }, { 9, Quality::Minor, -1 } };
    QuantizeGrid g = currentGrid();
    if (! g.active()) g.stepPpq = 1.0;

    const double spp = sampleRate * 60.0 / t.bpm;
    const double endPpq = t.ppq + numSamples / spp;
    if (t.ppq < lastTestLinePpq - 1.0e-6) lastTestLinePpq = -1.0e9;  // loop or relocation

    for (double line = g.nextLine (t.ppq); line < endPpq; line = g.nextLine (line + g.stepPpq * 0.25))
    {
        if (line <= lastTestLinePpq + 1.0e-7) continue;
        lastTestLinePpq = line;
        const int64_t at = juce::jlimit (blockStart, blockStart + numSamples - 1,
                                         blockStart + (int64_t) std::llround ((line - t.ppq) * spp));
        const int idx = (int) (((int64_t) std::floor (line / g.stepPpq + 0.5) % 4 + 4) % 4);

        Pending p;
        p.segment = ++testSegment;
        p.fireAt = p.gridAt = at;
        p.chord = cycle[idx];
        p.velocity = (uint8_t) juce::jlimit (1, 127, (int) pVelocity->load());
        addPending (p);
        readout.chord.store (p.chord.pack());
    }
}

void NoteCapAudioProcessor::firePending (juce::MidiBuffer& midi, const Transport& t, int64_t blockStart, int numSamples)
{
    const int64_t blockEnd = blockStart + numSamples;
    for (;;)
    {
        int next = -1;
        for (int i = 0; i < numPending; ++i)
            if (pending[(size_t) i].fireAt < blockEnd
                && (next < 0 || pending[(size_t) i].fireAt < pending[(size_t) next].fireAt
                    || (pending[(size_t) i].fireAt == pending[(size_t) next].fireAt && pending[(size_t) i].releaseOnly)))
                next = i;
        if (next < 0)
            return;

        const Pending p = pending[(size_t) next];
        pending[(size_t) next] = pending[(size_t) --numPending];
        fire (p, midi, t, blockStart, numSamples);
    }
}

void NoteCapAudioProcessor::fire (const Pending& p, juce::MidiBuffer& midi, const Transport& t,
                                  int64_t blockStart, int numSamples)
{
    const int offset = (int) juce::jlimit ((int64_t) 0, (int64_t) numSamples - 1, p.fireAt - blockStart);

    if (p.releaseOnly)
    {
        if (soundingSegment == p.segment)
            allNotesOff (midi, offset);
        return;
    }

    const auto style = (VoicingStyle) juce::jlimit (0, 2, (int) pVoicing->load());
    const auto voicing = makeVoicing (p.chord, style, (int) pOctave->load());
    const int channel = juce::jlimit (1, 16, (int) pChannel->load());
    auto contains = [] (const Voicing& v, int note) { return std::find (v.begin(), v.end(), note) != v.end(); };

    readout.sentChord.store (p.chord.pack());
    readout.lastWasLate.store (p.late);

    if (p.replacement && soundingSegment == p.segment && channel == soundingChannel && ! p.chord.isNone())
    {
        // Revised decision for the chord that's already sounding: keep common
        // tones held and only change the notes that differ.
        for (int note : sounding)
            if (! contains (voicing, note))
                midi.addEvent (juce::MidiMessage::noteOff (soundingChannel, note), offset);
        for (int note : voicing)
            if (! contains (sounding, note))
                midi.addEvent (juce::MidiMessage::noteOn (channel, note, p.velocity), offset);
    }
    else
    {
        allNotesOff (midi, offset);
        if (p.chord.isNone())
            return;
        for (int note : voicing)
            midi.addEvent (juce::MidiMessage::noteOn (channel, note, p.velocity), offset);
    }

    sounding = voicing;
    soundingChannel = channel;
    soundingSegment = p.segment;
    lastFiredSegment = p.segment;
    lastFiredGridAt = p.gridAt;
    lastFiredChord = p.chord;
    lastFiredVelocity = p.velocity;
    if (! p.replacement)
        readout.chordsSent.fetch_add (1);

    const int lenIdx = juce::jlimit (0, 4, (int) pLength->load());
    if (lengthSteps[lenIdx] > 0 && ! p.replacement)
    {
        const double spp = sampleRate * 60.0 / t.bpm;
        Pending off;
        off.segment = p.segment;
        off.releaseOnly = true;
        off.fireAt = std::max (p.fireAt + 1, p.gridAt + (int64_t) std::llround (lengthSteps[lenIdx] * stepPpq() * spp));
        off.gridAt = off.fireAt;
        addPending (off);
    }
}

void NoteCapAudioProcessor::allNotesOff (juce::MidiBuffer& midi, int sampleOffset)
{
    for (int note : sounding)
        midi.addEvent (juce::MidiMessage::noteOff (soundingChannel, note), sampleOffset);
    sounding.count = 0;
    soundingSegment = 0;
}

//==============================================================================
void NoteCapAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void NoteCapAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessorEditor* NoteCapAudioProcessor::createEditor()
{
    return new NoteCapAudioProcessorEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new NoteCapAudioProcessor();
}
