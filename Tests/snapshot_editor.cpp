// Renders the plugin editor offscreen to a PNG (for docs and visual checks),
// after feeding it a strummed chord so the displays have something to show.
//   Tests/run_integration.sh builds the library; then:
//   Tests/build/snapshot_editor out.png          (Chords mode, after G then Am)
//   Tests/build/snapshot_editor out.png notes    (Single notes mode, after a short picked line)
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Synth.h"

namespace
{
    struct PlayHead : juce::AudioPlayHead
    {
        PositionInfo info;
        juce::Optional<PositionInfo> getPosition() const override { return info; }
    };
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    const double fs = 48000.0;
    NoteCapAudioProcessor proc;
    proc.setRateAndBufferSizeDetails (fs, 256);
    const bool notes = argc > 2 && juce::String (argv[2]) == "notes";
    if (notes)
        if (auto* mode = proc.apvts.getParameter ("mode")) mode->setValueNotifyingHost (1.0f);
    PlayHead ph;
    proc.setPlayHead (&ph);
    proc.prepareToPlay (fs, 256);

    const auto audio = notes
        ? testsynth::renderGuitar ({ { 0.2, { 52 }, "" }, { 0.5, { 55 }, "" }, { 0.8, { 57 }, "" }, { 1.1, { 59 }, "" } },
                                   fs, 1.4, 4.0, 1, 0.0)
        : testsynth::renderGuitar ({ { 0.2, testsynth::guitarChord ("G"), "G" },
                                     { 1.2, testsynth::guitarChord ("Am"), "Am" } }, fs, 1.6, 4.0);
    juce::AudioBuffer<float> buf (4, 256);
    juce::MidiBuffer midi;
    for (size_t pos = 0; pos + 256 <= audio.size(); pos += 256)
    {
        ph.info.setIsPlaying (true);
        ph.info.setBpm (120.0);
        ph.info.setPpqPosition ((double) pos / 24000.0);
        buf.clear();
        for (int i = 0; i < 256; ++i) { buf.setSample (0, i, audio[pos + (size_t) i]); buf.setSample (1, i, audio[pos + (size_t) i]); }
        proc.processBlock (buf, midi);
    }

    std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
    const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
    juce::File out (argc > 1 ? juce::String (argv[1]) : juce::String ("editor.png"));
    out.deleteFile();
    juce::FileOutputStream stream (out);
    juce::PNGImageFormat().writeImageToStream (image, stream);
    std::cout << "wrote " << out.getFullPathName() << "\n";
    editor.reset();
    proc.setPlayHead (nullptr);
    return 0;
}
