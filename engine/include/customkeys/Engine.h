#pragma once

#include "customkeys/MidiEvent.h"
#include "customkeys/RenderStats.h"
#include "customkeys/Voice.h"

#include <cstdint>
#include <span>
#include <vector>

namespace ck {

/** The CustomKeys sound engine. Host-independent: the standalone app, AU and
    AAX wrappers all drive this same class.

    Threading: prepare() and setMaxPolyphony() allocate and must not run while
    process() is running. process() and reset() are real-time safe: they never
    allocate, lock, or touch the disk. */
class Engine
{
public:
    static constexpr int defaultPolyphony = 128;
    static constexpr int maxPolyphonyLimit = 256;

    /** Extra voices beyond the polyphony limit, used only for the short fade of
        stolen or damped notes so a new note never has to wait for one. */
    static constexpr int fadeHeadroom = 16;

    Engine();

    void prepare (double sampleRate, int maxPolyphony = defaultPolyphony);

    /** Renders one block. Outputs are overwritten. Events must be sorted by
        sampleOffset; offsets past the end of the block are applied at the end. */
    void process (float* const* outputs, int numChannels, int numSamples,
                  std::span<const MidiEvent> events) noexcept;

    /** Silences every voice immediately. */
    void reset() noexcept;

    int maxPolyphony() const noexcept { return polyphony; }
    int activeVoiceCount() const noexcept;
    bool sustainPedalDown() const noexcept { return sustainDown; }

    RenderStats& stats() noexcept { return renderStats; }
    const RenderStats& stats() const noexcept { return renderStats; }

private:
    void handleEvent (const MidiEvent& event) noexcept;
    void noteOn (int note, int velocity) noexcept;
    void noteOff (int note) noexcept;
    void setSustain (bool down) noexcept;
    void releaseAll() noexcept;
    void fadeAll() noexcept;

    Voice* findFreeVoice() noexcept;
    Voice* chooseVoiceToSteal() noexcept;
    int countSoundingVoices() const noexcept;

    std::vector<Voice> voices;
    int polyphony = defaultPolyphony;
    double sampleRate = 48000.0;
    uint64_t noteCounter = 0;
    bool sustainDown = false;
    RenderStats renderStats;
};

} // namespace ck
