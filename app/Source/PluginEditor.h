#pragma once

#include "PluginProcessor.h"

/** The panel: a header with live latency and load readouts and a panic button,
    an instrument bar (load a piano, or drop its file on the window), and a playable
    88-key keyboard. The full stage-panel layout (presets, effects) comes later. */
class CustomKeysEditor final : public juce::AudioProcessorEditor,
                               public juce::FileDragAndDropTarget,
                               private juce::Timer
{
public:
    explicit CustomKeysEditor (CustomKeysProcessor&);
    ~CustomKeysEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    void timerCallback() override;
    void chooseInstrument();

    CustomKeysProcessor& processor;

    juce::Label latencyLabel, loadLabel, voicesLabel;
    juce::TextButton resetPeaksButton { "Reset peaks" };
    juce::TextButton panicButton { "Panic" };
    juce::TextButton loadButton { "Load piano..." };
    juce::Label instrumentLabel;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::MidiKeyboardComponent keyboard;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CustomKeysEditor)
};
