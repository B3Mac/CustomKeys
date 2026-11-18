#pragma once

#include "PluginProcessor.h"

/** Milestone 1 panel: a header with live latency and load readouts, a panic
    button, and a playable 88-key keyboard. The full stage-panel layout (instrument
    slot, presets, effects) is built on top of this in later milestones. */
class CustomKeysEditor final : public juce::AudioProcessorEditor,
                               private juce::Timer
{
public:
    explicit CustomKeysEditor (CustomKeysProcessor&);
    ~CustomKeysEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    CustomKeysProcessor& processor;

    juce::Label latencyLabel, loadLabel, voicesLabel;
    juce::TextButton resetPeaksButton { "Reset peaks" };
    juce::TextButton panicButton { "Panic" };
    juce::MidiKeyboardComponent keyboard;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CustomKeysEditor)
};
