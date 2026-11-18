#include "PluginProcessor.h"
#include "PluginEditor.h"

#include "customkeys/NordPiano.h"
#include "customkeys/SampleLibrary.h"

CustomKeysProcessor::CustomKeysProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    startTimerHz (10);
}

CustomKeysProcessor::~CustomKeysProcessor()
{
    stopTimer();
    stopLoader();
}

namespace
{
bool isNordPiano (const juce::File& file)
{
    return file.existsAsFile() && file.hasFileExtension ("npno");
}
} // namespace

bool CustomKeysProcessor::canLoad (const juce::File& file)
{
    // .npno pianos, .sfz instruments, single WAV/AIFF/FLAC files and folders of them.
    return isNordPiano (file) || ck::library::canLoad (file.getFullPathName().toStdString());
}

void CustomKeysProcessor::stopLoader()
{
    cancelLoad.store (true);
    if (loader.joinable())
        loader.join();
    cancelLoad.store (false);
}

void CustomKeysProcessor::loadInstrument (const juce::File& file)
{
    stopLoader(); // a newer choice replaces a load still running

    {
        std::lock_guard lock (resultLock);
        result.reset();
    }

    if (! canLoad (file))
    {
        status = file.getFileName() + " isn't something CustomKeys can open. Try a .npno piano, an .sfz file, "
                 "a WAV, AIFF or FLAC file, or a folder of them.";
        return;
    }

    requestedFile = file;
    loading.store (true);
    progress.store (0.0f);
    status = "Loading " + (file.isDirectory() ? file.getFileName() : file.getFileNameWithoutExtension()) + "...";

    loader = std::thread ([this, file]
    {
        const auto path = file.getFullPathName().toStdString();
        std::string error;
        std::shared_ptr<ck::Instrument> instrument;
        juce::StringArray warnings;

        if (isNordPiano (file))
        {
            ck::nord::LoadOptions options;
            options.progress = &progress;
            options.cancel = &cancelLoad;
            instrument = ck::nord::loadPiano (path, error, options);
        }
        else
        {
            ck::library::LoadReport report;
            ck::library::LoadOptions options;
            options.progress = &progress;
            options.cancel = &cancelLoad;
            instrument = ck::library::load (path, error, &report, options);
            for (const auto& warning : report.warnings)
                warnings.add (juce::String (warning));
        }

        std::lock_guard lock (resultLock);
        if (! cancelLoad.load())
            result = LoadResult { std::move (instrument), juce::String (error), file, std::move (warnings) };
        loading.store (false);
    });
}

void CustomKeysProcessor::timerCallback()
{
    std::optional<LoadResult> done;
    {
        std::lock_guard lock (resultLock);
        done.swap (result);
    }

    if (done.has_value())
    {
        if (loader.joinable())
            loader.join();

        if (done->instrument != nullptr)
        {
            const auto megabytes = static_cast<double> (done->instrument->sampleBytes()) / (1024.0 * 1024.0);
            engine.setInstrument (done->instrument);
            loadedFile = done->file;
            status = juce::String (done->instrument->name) + "  ("
                     + juce::String (done->instrument->samples.size()) + " recordings, "
                     + juce::String (megabytes, 0) + " MB)";

            // Say what was skipped or approximated; the full list goes to the debug log.
            if (! done->warnings.isEmpty())
            {
                status += "  " + juce::String (done->warnings.size())
                          + (done->warnings.size() == 1 ? " note: " : " notes: ") + done->warnings[0];
                DBG ("Load notes:\n" << done->warnings.joinIntoString ("\n"));
            }
        }
        else
        {
            status = "Couldn't load " + done->file.getFileName() + ": " + done->error;
        }
    }

    engine.collectGarbage();
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
    juce::ValueTree state ("CustomKeysState");
    state.setProperty ("version", 1, nullptr);
    if (requestedFile != juce::File())
        state.setProperty ("instrumentFile", requestedFile.getFullPathName(), nullptr);
    juce::MemoryOutputStream stream (destData, false);
    state.writeToStream (stream);
}

void CustomKeysProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto state = juce::ValueTree::readFromData (data, static_cast<size_t> (sizeInBytes));
    const auto path = state.getProperty ("instrumentFile").toString();

    // Reopen the last instrument. The file is read in place each time; nothing is copied.
    if (path.isNotEmpty() && juce::File::isAbsolutePath (path) && juce::File (path) != requestedFile)
    {
        const juce::File file (path);
        if (file.exists())
            loadInstrument (file);
        else
            status = "Couldn't find " + file.getFileName() + ". Is its drive connected?";
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new CustomKeysProcessor();
}
