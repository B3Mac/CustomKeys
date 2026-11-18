#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "customkeys/Engine.h"

#include <array>
#include <atomic>

/** The JUCE wrapper around the CustomKeys engine. This one class is what the
    standalone app, AU, VST3 and AAX builds all host, so it holds no format-specific code. */
class CustomKeysProcessor final : public juce::AudioProcessor
{
public:
    CustomKeysProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.5; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    /** Notes from the on-screen keyboard are merged into the MIDI stream here. */
    juce::MidiKeyboardState keyboardState;

    /** Thread-safe: the next audio block silences every voice. */
    void panic() noexcept { panicRequested.store (true); }

    ck::RenderStats& renderStats() noexcept { return engine.stats(); }
    int lastBlockSize() const noexcept { return blockSize.load (std::memory_order_relaxed); }
    double currentSampleRate() const noexcept { return getSampleRate(); }

private:
    ck::Engine engine;

    // Preallocated so the audio thread never allocates while translating MIDI.
    std::array<ck::MidiEvent, 4096> eventScratch {};

    std::atomic<bool> panicRequested { false };
    std::atomic<int> blockSize { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CustomKeysProcessor)
};
