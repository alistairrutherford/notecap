#include "PluginEditor.h"

using namespace notecap;
using namespace notecap::ui;

//==============================================================================
LookAndFeel::LookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, colours::background);
    setColour (juce::Label::textColourId, colours::text);
    setColour (juce::Slider::textBoxTextColourId, colours::text);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxBackgroundColourId, colours::background);
    setColour (juce::ComboBox::backgroundColourId, colours::background);
    setColour (juce::ComboBox::outlineColourId, colours::outline);
    setColour (juce::ComboBox::textColourId, colours::text);
    setColour (juce::ComboBox::arrowColourId, colours::dim);
    setColour (juce::PopupMenu::backgroundColourId, colours::panel);
    setColour (juce::PopupMenu::textColourId, colours::text);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, colours::accent.withAlpha (0.3f));
    setColour (juce::ToggleButton::textColourId, colours::text);
    setColour (juce::Label::outlineColourId, colours::outline);
    setColour (juce::TextEditor::outlineColourId, colours::outline);
    setColour (juce::TextEditor::focusedOutlineColourId, colours::accent);
    setColour (juce::TextEditor::backgroundColourId, colours::background);
}

void LookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float,
                                    juce::Slider::SliderStyle, juce::Slider& s)
{
    const auto track = juce::Rectangle<float> ((float) x, (float) y + h * 0.5f - 2.0f, (float) w, 4.0f);
    g.setColour (colours::outline);
    g.fillRoundedRectangle (track, 2.0f);

    // Bipolar ranges (e.g. port offset) fill from zero.
    const double zero = s.getMinimum() < 0.0 && s.getMaximum() > 0.0 ? s.valueToProportionOfLength (0.0) : 0.0;
    const float zx = (float) x + (float) zero * (float) w;
    g.setColour (colours::accent.withAlpha (s.isEnabled() ? 1.0f : 0.4f));
    g.fillRoundedRectangle (juce::Rectangle<float> (std::min (zx, pos), track.getY(), std::abs (pos - zx), 4.0f), 2.0f);
    g.fillEllipse (pos - 6.0f, (float) y + h * 0.5f - 6.0f, 12.0f, 12.0f);
}

void LookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool highlighted, bool)
{
    const auto r = b.getLocalBounds().toFloat();
    const auto sw = juce::Rectangle<float> (r.getX(), r.getCentreY() - 8.0f, 30.0f, 16.0f);
    const bool on = b.getToggleState();
    g.setColour (on ? colours::accent : colours::outline.brighter (highlighted ? 0.2f : 0.0f));
    g.fillRoundedRectangle (sw, 8.0f);
    g.setColour (on ? colours::background : colours::dim);
    g.fillEllipse (on ? sw.getRight() - 14.0f : sw.getX() + 2.0f, sw.getY() + 2.0f, 12.0f, 12.0f);
    g.setColour (colours::text);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText (b.getButtonText(), r.withTrimmedLeft (38.0f), juce::Justification::centredLeft);
}

void LookAndFeel::drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox&)
{
    const auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) w, (float) h).reduced (0.5f);
    g.setColour (colours::background);
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (colours::outline);
    g.drawRoundedRectangle (r, 4.0f, 1.0f);
    juce::Path arrow;
    const float ax = (float) w - 14.0f, ay = (float) h * 0.5f;
    arrow.addTriangle (ax - 4.0f, ay - 2.0f, ax + 4.0f, ay - 2.0f, ax, ay + 3.0f);
    g.setColour (colours::dim);
    g.fillPath (arrow);
}

//==============================================================================
static void drawPanel (juce::Graphics& g, juce::Rectangle<float> r)
{
    g.setColour (colours::panel);
    g.fillRoundedRectangle (r, 8.0f);
    g.setColour (colours::outline);
    g.drawRoundedRectangle (r.reduced (0.5f), 8.0f, 1.0f);
}

void ChordDisplay::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    drawPanel (g, r);
    r = r.reduced (14.0f, 10.0f);

    g.setColour (colours::dim);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawText ("DETECTED", r.removeFromTop (14.0f), juce::Justification::centredLeft);

    const auto chord = Chord::unpack (proc.readout.chord.load());
    const float conf = proc.readout.confidence.load();
    g.setColour (chord.isNone() ? colours::dim : colours::text);
    g.setFont (juce::FontOptions (chord.name().size() > 5 ? 50.0f : 64.0f, juce::Font::bold));
    g.drawFittedText (chord.isNone() ? juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x94")) : juce::String (chord.name()),
                      r.removeFromTop (84.0f).toNearestInt(), juce::Justification::centredLeft, 1);

    const auto prev = Chord::unpack (proc.readout.previousChord.load());
    g.setColour (colours::dim);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText (prev.isNone() ? juce::String() : "previous  " + juce::String (prev.name()),
                r.removeFromTop (20.0f), juce::Justification::centredLeft);

    r.removeFromTop (8.0f);
    auto bar = r.removeFromTop (6.0f);
    g.setColour (colours::outline);
    g.fillRoundedRectangle (bar, 3.0f);
    const float level = chord.isNone() ? 0.0f : juce::jlimit (0.0f, 1.0f, (conf - 0.4f) / 0.6f);
    g.setColour (level > 0.5f ? colours::good : colours::warn);
    g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * level), 3.0f);
    g.setColour (colours::dim);
    g.setFont (juce::FontOptions (11.0f));
    g.drawText ("confidence", r.removeFromTop (16.0f), juce::Justification::centredLeft);
}

void ChromaDisplay::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    drawPanel (g, r);
    r = r.reduced (14.0f, 10.0f);

    g.setColour (colours::dim);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawText ("PITCH CLASSES", r.removeFromTop (14.0f), juce::Justification::centredLeft);
    r.removeFromTop (6.0f);

    const auto chord = Chord::unpack (proc.readout.chord.load());
    bool inChord[12] {};
    for (int pc : chord.pitchClasses()) inChord[pc] = true;
    const int bass = proc.readout.bassPc.load();

    auto labels = r.removeFromBottom (16.0f);
    const float w = r.getWidth() / 12.0f;
    float maxV = 0.2f;
    for (auto& v : proc.readout.chroma) maxV = std::max (maxV, v.load());

    for (int i = 0; i < 12; ++i)
    {
        const float v = proc.readout.chroma[(size_t) i].load() / maxV;
        auto col = juce::Rectangle<float> (r.getX() + i * w + 3.0f, r.getY(), w - 6.0f, r.getHeight());
        g.setColour (colours::outline.withAlpha (0.5f));
        g.fillRoundedRectangle (col, 3.0f);
        g.setColour (inChord[i] ? colours::accent : colours::dim.withAlpha (0.7f));
        g.fillRoundedRectangle (col.withTop (col.getBottom() - col.getHeight() * juce::jlimit (0.0f, 1.0f, v)), 3.0f);
        if (i == bass)
        {
            g.setColour (colours::warn);
            g.drawRoundedRectangle (col, 3.0f, 1.5f);
        }
        g.setColour (inChord[i] ? colours::text : colours::dim);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (pitchClassName (i), juce::Rectangle<float> (r.getX() + i * w, labels.getY(), w, 16.0f),
                    juce::Justification::centred);
    }
}

void StatusDisplay::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    drawPanel (g, r);
    r = r.reduced (14.0f, 10.0f);

    g.setColour (colours::dim);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawText ("STATUS", r.removeFromTop (14.0f), juce::Justification::centredLeft);
    r.removeFromTop (4.0f);

    auto row = [&] (const juce::String& key, const juce::String& value, juce::Colour c)
    {
        auto line = r.removeFromTop (21.0f);
        g.setFont (juce::FontOptions (12.0f));
        g.setColour (colours::dim);
        g.drawText (key, line.removeFromLeft (70.0f), juce::Justification::centredLeft);
        g.setColour (c);
        g.drawText (value, line, juce::Justification::centredLeft);
    };

    const auto& out = proc.getMidiOut();
    const bool wantPort = proc.apvts.getRawParameterValue ("virtualPort")->load() > 0.5f;
    row ("MIDI port", out.isOpen() ? out.getName()
                                   : (wantPort ? "failed to open (error " + juce::String (out.getLastError()) + ")" : "off"),
         out.isOpen() ? colours::good : (wantPort ? colours::bad : colours::dim));

    const bool playing = proc.readout.playing.load();
    row ("Transport", (playing ? "playing  " : "stopped  ") + juce::String (proc.readout.bpm.load(), 1) + " BPM",
         playing ? colours::good : colours::dim);

    const bool useSide = proc.apvts.getRawParameterValue ("source")->load() < 0.5f;
    const float level = proc.readout.levelDb.load();
    const float gate = proc.apvts.getRawParameterValue ("gate")->load();
    juce::String inputText = useSide ? "sidechain" : "main input";
    juce::Colour inputCol = colours::text;
    if (useSide && ! proc.readout.sidechainConnected.load()) { inputText = "sidechain not connected"; inputCol = colours::bad; }
    else if (level < -90.0f) { inputText << "  (silent)"; inputCol = colours::warn; }
    row ("Input", inputText, inputCol);

    // Level meter with the gate marked.
    auto meter = r.removeFromTop (12.0f).withTrimmedLeft (70.0f).withTrimmedBottom (4.0f);
    auto toX = [&] (float db) { return meter.getX() + meter.getWidth() * juce::jlimit (0.0f, 1.0f, (db + 90.0f) / 90.0f); };
    g.setColour (colours::outline);
    g.fillRoundedRectangle (meter, 2.0f);
    g.setColour (level > gate ? colours::good : colours::dim);
    g.fillRoundedRectangle (meter.withRight (toX (level)), 2.0f);
    g.setColour (colours::warn);
    g.fillRect (toX (gate) - 1.0f, meter.getY() - 2.0f, 2.0f, meter.getHeight() + 4.0f);
    r.removeFromTop (2.0f);

    const float lat = proc.readout.latencyMs.load();
    row ("Latency", lat > 0.0f ? juce::String (juce::roundToInt (lat)) + " ms detect" : juce::String ("-"), colours::text);
    const float cents = proc.readout.tuningCents.load();
    row ("Tuning", (cents >= 0 ? "+" : "") + juce::String (juce::roundToInt (cents)) + " cents", colours::text);

    const auto sent = Chord::unpack (proc.readout.sentChord.load());
    row ("Sent", (sent.isNone() ? juce::String ("-") : juce::String (sent.name()))
                     + (proc.readout.lastWasLate.load() && ! sent.isNone() ? "  (late)" : "")
                     + "   " + juce::String (proc.readout.chordsSent.load()) + " total",
         colours::accent);
}

//==============================================================================
NoteCapAudioProcessorEditor::NoteCapAudioProcessorEditor (NoteCapAudioProcessor& p)
    : AudioProcessorEditor (&p), proc (p), chordDisplay (p), chromaDisplay (p), statusDisplay (p)
{
    setLookAndFeel (&lnf);
    addAndMakeVisible (chordDisplay);
    addAndMakeVisible (chromaDisplay);
    addAndMakeVisible (statusDisplay);

    auto& detect = groups[0];
    detect.title = "DETECT";
    addCombo (detect, "source", "Source");
    addSlider (detect, "gate", "Gate");
    addSlider (detect, "sensitivity", "Sensitivity");
    addSlider (detect, "minChord", "Min change");
    addToggle (detect, "sevenths", "7th chords");
    addToggle (detect, "suspended", "Sus chords");
    addToggle (detect, "dimAug", "Dim / aug chords");
    addToggle (detect, "retrigger", "Retrigger on re-strum");

    auto& quant = groups[1];
    quant.title = "QUANTIZE";
    addCombo (quant, "grid", "Grid");
    addCombo (quant, "qmode", "Mode");
    addSlider (quant, "swing", "Swing");
    addCombo (quant, "length", "Length");

    auto& output = groups[2];
    output.title = "OUTPUT";
    addCombo (output, "voicing", "Voicing");
    addSlider (output, "octave", "Octave");
    addCombo (output, "velMode", "Velocity");
    addSlider (output, "velocity", "Fixed vel.");
    addSlider (output, "channel", "Channel");
    addSlider (output, "offsetMs", "Port offset");
    addToggle (output, "virtualPort", "Virtual MIDI port");
    addToggle (output, "testMode", "Test chords (C F G Am)");

    setSize (880, 530);
    startTimerHz (30);
}

NoteCapAudioProcessorEditor::~NoteCapAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
}

void NoteCapAudioProcessorEditor::addSlider (Group& grp, const juce::String& id, const juce::String& label)
{
    auto c = std::make_unique<Control>();
    c->label = label;
    auto s = std::make_unique<juce::Slider> (juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight);
    s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 54, 20);
    c->sliderAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, id, *s);
    addAndMakeVisible (*s);
    c->component = std::move (s);
    grp.controls.push_back (std::move (c));
}

void NoteCapAudioProcessorEditor::addCombo (Group& grp, const juce::String& id, const juce::String& label)
{
    auto c = std::make_unique<Control>();
    c->label = label;
    auto box = std::make_unique<juce::ComboBox>();
    if (auto* param = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter (id)))
        box->addItemList (param->choices, 1);
    c->comboAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, id, *box);
    addAndMakeVisible (*box);
    c->component = std::move (box);
    grp.controls.push_back (std::move (c));
}

void NoteCapAudioProcessorEditor::addToggle (Group& grp, const juce::String& id, const juce::String& label)
{
    auto c = std::make_unique<Control>();
    auto b = std::make_unique<juce::ToggleButton> (label);
    c->buttonAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, id, *b);
    addAndMakeVisible (*b);
    c->component = std::move (b);
    grp.controls.push_back (std::move (c));
}

void NoteCapAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);

    for (const auto& grp : groups)
    {
        drawPanel (g, grp.bounds.toFloat());
        g.setColour (colours::dim);
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText (grp.title, grp.bounds.reduced (14, 10).removeFromTop (14), juce::Justification::centredLeft);

        g.setFont (juce::FontOptions (13.0f));
        g.setColour (colours::text);
        for (const auto& c : grp.controls)
            if (c->label.isNotEmpty())
                g.drawText (c->label, c->component->getBounds().withX (grp.bounds.getX() + 14).withWidth (84),
                            juce::Justification::centredLeft);
    }

    // Recording tip under the quantize controls.
    const auto& q = groups[1];
    auto tip = q.bounds.reduced (14, 12);
    tip.setTop (q.controls.back()->component->getBottom() + 16);
    g.setColour (colours::dim);
    g.setFont (juce::FontOptions (12.0f));
    g.drawFittedText ("Nearest snaps each chord to the grid line closest to where it was played. "
                      "If that line has already passed, the chord is sent at once and time-stamped on the line.\n\n"
                      "To record: arm a MIDI track whose MIDI From is the NoteCap port shown in Status. "
                      "Also set Live's Record Quantization to the same grid.",
                      tip, juce::Justification::topLeft, 12);
}

void NoteCapAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (12);
    auto top = area.removeFromTop (190);
    chordDisplay.setBounds (top.removeFromLeft (260));
    top.removeFromLeft (10);
    statusDisplay.setBounds (top.removeFromRight (300));
    top.removeFromRight (10);
    chromaDisplay.setBounds (top);

    area.removeFromTop (10);
    const int gw = (area.getWidth() - 20) / 3;
    for (size_t i = 0; i < groups.size(); ++i)
    {
        auto& grp = groups[i];
        grp.bounds = area.removeFromLeft (gw);
        area.removeFromLeft (10);

        auto inner = grp.bounds.reduced (14, 10);
        inner.removeFromTop (22);
        for (auto& c : grp.controls)
        {
            auto row = inner.removeFromTop (30).reduced (0, 3);
            c->component->setBounds (c->label.isNotEmpty() ? row.withTrimmedLeft (88) : row);
        }
    }
}

void NoteCapAudioProcessorEditor::timerCallback()
{
    chordDisplay.repaint();
    chromaDisplay.repaint();
    statusDisplay.repaint();
}
