#pragma once

#include "customkeys/Instrument.h"
#include "customkeys/MidiEvent.h"
#include "customkeys/RenderStats.h"
#include "customkeys/Voice.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace ck {

/** The CustomKeys sound engine. Host-independent: the standalone app, AU and
    AAX wrappers all drive this same class.

    Threading: prepare() allocates and must not run while process() is running.
    setInstrument() and collectGarbage() belong to one non-audio thread (the
    message thread) and may run while audio is playing. process() and reset() are
    real-time safe: they never allocate, lock, free memory or touch the disk. */
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

    /** Swaps in a new instrument, or the test tone when null. The audio thread
        picks it up at the start of its next block and silences the old notes.
        The engine keeps the instrument alive until the audio thread has let go of it. */
    void setInstrument (std::shared_ptr<const Instrument> instrument);

    /** The instrument most recently passed to setInstrument (message thread). */
    std::shared_ptr<const Instrument> instrument() const;

    /** Frees instruments the audio thread no longer uses. Call it now and then from
        the thread that calls setInstrument; setInstrument also calls it. */
    void collectGarbage();

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

    void syncInstrument() noexcept;

    std::vector<Voice> voices;

    // Instrument hand-over. The message thread writes `requested`; the audio thread
    // reads it each block and reports what it plays in `inUse`. Once `inUse` equals
    // `requested`, the audio thread can never reach an older one, so those are freed.
    std::vector<std::shared_ptr<const Instrument>> owned; // message thread only
    std::atomic<const Instrument*> requested { nullptr };
    std::atomic<const Instrument*> inUse { nullptr };
    const Instrument* current = nullptr; // audio thread only

    int polyphony = defaultPolyphony;
    double sampleRate = 48000.0;
    uint64_t noteCounter = 0;
    bool sustainDown = false;
    RenderStats renderStats;
};

} // namespace ck
