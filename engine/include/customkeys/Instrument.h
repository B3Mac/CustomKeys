#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ck {

/** One recording held in memory, ready for the audio thread.

    Samples are stored as 16-bit integers to halve the memory of a large piano;
    `gain` turns them back into floats at full scale. Everything is read-only once
    the instrument has been handed to the engine. */
struct Sample
{
    std::vector<int16_t> data; ///< Interleaved frames.
    int channels = 1;          ///< 1 or 2.
    int64_t frames = 0;
    double sampleRate = 44100.0;
    float gain = 1.0f / 32768.0f;

    /** Sustain loop in frames; no loop when loopLength is 0. A looping sample
        keeps playing for as long as the key or pedal holds it. */
    int64_t loopStart = 0;
    int64_t loopLength = 0;

    /** How fast the level falls once playback is inside the loop, as the natural
        log of the level change per second (0 holds the level, negative decays). */
    double loopDecayPerSecond = 0.0;

    bool loops() const noexcept { return loopLength > 0 && loopStart + loopLength <= frames; }
};

/** A key range played by the recordings of one sampled note. */
struct Zone
{
    /** The key at which a recording plays at its own speed. Can be fractional
        (in semitones) when a format records slightly off the key-map root. */
    double rootKey = 60.0;

    /** Index into Instrument::samples for each velocity 0-127, or -1 for silence.
        A table rather than ranges so any format's layer rule fits, and the audio
        thread picks a layer with one lookup. */
    std::array<int32_t, 128> sampleForVelocity {};
};

/** A playable multisampled instrument: what a loader builds and the engine plays.
    Built on a background thread, then handed to Engine::setInstrument. */
struct Instrument
{
    std::string name;
    std::vector<Sample> samples;
    std::vector<Zone> zones;

    /** Zone index for each MIDI key, or -1 where the instrument has no sound. */
    std::array<int16_t, 128> zoneForKey {};

    /** Linear level for each velocity 0-127, applied on top of the sample's gain. */
    std::array<float, 128> velocityGain {};

    /** Key-up fade (seconds to fall by 1/e) once neither key nor pedal holds a note. */
    float releaseTimeConstant = 0.05f;

    /** Mix level, leaving headroom so chords do not clip. */
    float outputGain = 0.35f;

    /** Total memory of the recordings, for the UI. */
    size_t sampleBytes() const noexcept
    {
        size_t total = 0;
        for (const auto& s : samples)
            total += s.data.size() * sizeof (int16_t);
        return total;
    }
};

} // namespace ck
