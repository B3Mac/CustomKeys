#include "PluginProcessor.h"
#include "PluginEditor.h"

CustomKeysProcessor::CustomKeysProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
}

void CustomKeysProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, ck::Engine::defaultPolyphony);
    keyboardState.reset();
    blockSize.store (samplesPerBlock);
}

bool CustomKeysProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
}

void CustomKeysProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    blockSize.store (numSamples, std::memory_order_relaxed);

    // Adds on-screen keyboard presses to the incoming MIDI, and lights up keys played on hardware.
    keyboardState.processNextMidiBuffer (midi, 0, numSamples, true);

    size_t count = 0;
    if (panicRequested.exchange (false))
        eventScratch[count++] = ck::MidiEvent::controller (0, 120, 0);

    for (const auto metadata : midi)
    {
        if (count == eventScratch.size())
            break;
        if (metadata.numBytes < 1 || metadata.numBytes > 3)
            continue; // SysEx and other long messages are not used by the engine.

        const auto* bytes = metadata.data;
        eventScratch[count++] = { static_cast<uint32_t> (juce::jlimit (0, juce::jmax (0, numSamples - 1), metadata.samplePosition)),
                                  bytes[0],
                                  metadata.numBytes > 1 ? bytes[1] : uint8_t { 0 },
                                  metadata.numBytes > 2 ? bytes[2] : uint8_t { 0 } };
    }

    engine.process (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), numSamples,
                    std::span<const ck::MidiEvent> (eventScratch.data(), count));
}

juce::AudioProcessorEditor* CustomKeysProcessor::createEditor()
{
    return new CustomKeysEditor (*this);
}

void CustomKeysProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // Milestone 1 has no settings to save yet. Writing a versioned tree now
    // means hosts store a valid state from day one.
    juce::ValueTree state ("CustomKeysState");
    state.setProperty ("version", 1, nullptr);
    juce::MemoryOutputStream stream (destData, false);
    state.writeToStream (stream);
}

void CustomKeysProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    juce::ignoreUnused (juce::ValueTree::readFromData (data, static_cast<size_t> (sizeInBytes)));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new CustomKeysProcessor();
}
