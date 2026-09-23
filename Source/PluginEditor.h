#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

namespace notecap::ui
{
    namespace colours
    {
        const juce::Colour background { 0xff15171c };
        const juce::Colour panel      { 0xff1f232b };
        const juce::Colour outline    { 0xff2e333d };
        const juce::Colour text       { 0xffe4e7ec };
        const juce::Colour dim        { 0xff8a93a3 };
        const juce::Colour accent     { 0xff4fc3f7 };
        const juce::Colour good       { 0xff66d19e };
        const juce::Colour warn       { 0xfff2b55c };
        const juce::Colour bad        { 0xffef6f6c };
    }

    class LookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        LookAndFeel();
        void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h, float pos, float minPos, float maxPos,
                               juce::Slider::SliderStyle, juce::Slider&) override;
        void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
        void drawComboBox (juce::Graphics&, int w, int h, bool down, int bx, int by, int bw, int bh, juce::ComboBox&) override;
    };

    // Big current-chord display with confidence bar.
    class ChordDisplay : public juce::Component
    {
    public:
        explicit ChordDisplay (NoteCapAudioProcessor& p) : proc (p) {}
        void paint (juce::Graphics&) override;
    private:
        NoteCapAudioProcessor& proc;
    };

    // 12 pitch-class bars.
    class ChromaDisplay : public juce::Component
    {
    public:
        explicit ChromaDisplay (NoteCapAudioProcessor& p) : proc (p) {}
        void paint (juce::Graphics&) override;
    private:
        NoteCapAudioProcessor& proc;
    };

    // Port / transport / latency / level status panel.
    class StatusDisplay : public juce::Component
    {
    public:
        explicit StatusDisplay (NoteCapAudioProcessor& p) : proc (p) {}
        void paint (juce::Graphics&) override;
    private:
        NoteCapAudioProcessor& proc;
    };
}

class NoteCapAudioProcessorEditor : public juce::AudioProcessorEditor,
                                    private juce::Timer
{
public:
    explicit NoteCapAudioProcessorEditor (NoteCapAudioProcessor&);
    ~NoteCapAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    struct Control
    {
        juce::String label;
        std::unique_ptr<juce::Component> component;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sliderAttachment;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> comboAttachment;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> buttonAttachment;
    };

    struct Group
    {
        juce::String title;
        std::vector<std::unique_ptr<Control>> controls;
        juce::Rectangle<int> bounds;
    };

    void addSlider (Group&, const juce::String& paramId, const juce::String& label);
    void addCombo (Group&, const juce::String& paramId, const juce::String& label);
    void addToggle (Group&, const juce::String& paramId, const juce::String& label);

    NoteCapAudioProcessor& proc;
    notecap::ui::LookAndFeel lnf;
    notecap::ui::ChordDisplay chordDisplay;
    notecap::ui::ChromaDisplay chromaDisplay;
    notecap::ui::StatusDisplay statusDisplay;
    std::array<Group, 3> groups;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NoteCapAudioProcessorEditor)
};
