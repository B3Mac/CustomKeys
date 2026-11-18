#pragma once

#include "customkeys/Instrument.h"

#include <cstdint>

namespace ck {

/** One sounding note.

    A voice plays either a recording from the loaded instrument (startSample) or,
    when no instrument is loaded, a test tone (start): a sine with a touch of second
    harmonic and a piano-like decay, so the MIDI-to-audio path can be checked
    without samples. The envelope, stage and stealing logic are shared. */
class Voice
{
public:
    enum class Stage : uint8_t
    {
        Idle,      ///< Free for a new note.
        Playing,   ///< Key or pedal is holding the note.
        Releasing, ///< Key and pedal are up; natural release tail.
        Fading     ///< Being stolen or damped: short linear fade to silence.
    };

    static constexpr float releaseTimeConstant = 0.04f; ///< Seconds; the release tail reaches -80 dB after about 0.37 s.
    static constexpr float fadeSeconds    = 0.004f; ///< Steal and damp fade, short enough to be inaudible as a click.
    static constexpr float attackSeconds  = 0.002f;

    void start (int midiNote, float velocity01, double sampleRate, uint64_t order) noexcept;

    /** Plays `sample` for `midiNote`. `pitchSemitones` is the transposition from the
        recording; `gain` the note's level; `releaseTimeConstant` the key-up fade. */
    void startSample (int midiNote, const Sample& sample, double pitchSemitones, float gain,
                      float releaseTimeConstant, double sampleRate, uint64_t order) noexcept;
    void release() noexcept;
    void fadeOut() noexcept;
    void kill() noexcept;

    /** Adds this voice into the outputs. Either pointer may be null. */
    void render (float* left, float* right, int numSamples) noexcept;

    Stage stage() const noexcept       { return currentStage; }
    bool isIdle() const noexcept       { return currentStage == Stage::Idle; }
    bool isFading() const noexcept     { return currentStage == Stage::Fading; }
    int note() const noexcept          { return midiNote; }
    uint64_t startOrder() const noexcept { return order; }

    /** Current output amplitude, used to steal the quietest voice. */
    float level() const noexcept { return envelope * gain; }

    bool keyDown = false;   ///< The key that started this voice is still held.
    bool sustained = false; ///< Key is up but the sustain pedal is holding the note.

private:
    Stage currentStage = Stage::Idle;
    int midiNote = -1;
    uint64_t order = 0;

    // Quadrature oscillator: (cosPhase, sinPhase) rotated by (cosStep, sinStep) each sample.
    float cosPhase = 1.0f, sinPhase = 0.0f;
    float cosStep = 1.0f, sinStep = 0.0f;
    int samplesSinceNormalise = 0;

    float gain = 0.0f;
    float envelope = 0.0f;
    float attackIncrement = 0.0f;
    float decayCoefficient = 1.0f;
    float releaseCoefficient = 1.0f;
    float fadeDecrement = 0.0f;
    bool attacking = false;

    double sampleRate = 48000.0;

    // Sample playback (null for the test tone).
    float readFrame (int64_t frame, int channel) const noexcept;
    void renderSample (float* left, float* right, int numSamples) noexcept;

    const Sample* sample = nullptr;
    double position = 0.0;  ///< In frames of the recording.
    double increment = 1.0; ///< Frames of the recording per output sample.
    float loopDecayCoefficient = 1.0f;
};

} // namespace ck
