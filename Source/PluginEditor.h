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

    // Ableton-style Info View: a strip along the bottom describing whatever the mouse is over.
    class InfoView : public juce::Component
    {
    public:
        void show (const juce::String& title, const juce::String& text);
        void paint (juce::Graphics&) override;
        const juce::String& getTitle() const noexcept { return title; }
    private:
        juce::String title, text;
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

    void mouseEnter (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    // Info View: the key ("grid", "display.status", ...) of the area at `p` (editor
    // coordinates), its title, and showing one directly (used by the snapshot tool).
    juce::String infoKeyAt (juce::Point<int> p) const;
    static juce::String infoTitle (const juce::String& key);
    static juce::String infoText (const juce::String& key);
    void showInfoFor (const juce::String& key);
    const juce::String& shownInfoTitle() const noexcept { return infoView.getTitle(); }

    // Chord-only controls are dimmed in Single notes mode (checked on the UI timer).
    static bool isChordOnly (const juce::String& paramId);
    bool isDimmed (const juce::String& paramId) const;
    void refreshForMode();
    juce::String quantizeTip() const;   // the note under the Quantize controls ("chord" / "note")

private:
    void timerCallback() override;
    void updateInfo (const juce::MouseEvent&);

    struct Control
    {
        juce::String paramId;
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
    notecap::ui::InfoView infoView;
    std::array<Group, 3> groups;

    struct InfoRegion { juce::Rectangle<int> area; juce::String key; };
    std::vector<InfoRegion> infoRegions;   // most specific first; rebuilt in resized()
    juce::String hoveredKey;
    bool dimmedForNotes = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NoteCapAudioProcessorEditor)
};
