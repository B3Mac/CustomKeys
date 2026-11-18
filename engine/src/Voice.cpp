#include "customkeys/Voice.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ck {

namespace {
    constexpr float silenceThreshold = 1.0e-4f; // -80 dB

    float coefficientForTimeConstant (double seconds, double sampleRate) noexcept
    {
        return static_cast<float> (std::exp (-1.0 / (seconds * sampleRate)));
    }
}

void Voice::start (int note, float velocity01, double rate, uint64_t startOrder) noexcept
{
    sampleRate = rate;
    midiNote = note;
    order = startOrder;
    keyDown = true;
    sustained = false;
    currentStage = Stage::Playing;

    const double frequency = 440.0 * std::pow (2.0, (note - 69) / 12.0);
    const double step = 2.0 * std::numbers::pi * frequency / sampleRate;
    cosStep = static_cast<float> (std::cos (step));
    sinStep = static_cast<float> (std::sin (step));
    cosPhase = 1.0f;
    sinPhase = 0.0f;
    samplesSinceNormalise = 0;

    // Squared velocity feels natural on most keyboards; 0.2 leaves headroom for chords.
    gain = 0.2f * velocity01 * velocity01;

    envelope = 0.0f;
    attacking = true;
    attackIncrement = static_cast<float> (1.0 / (attackSeconds * sampleRate));

    // Low notes ring longer, like a real piano: about 8 s at A0 down to 1 s at C8.
    const double keyPosition = std::clamp ((note - 21) / 87.0, 0.0, 1.0);
    const double decaySeconds = 8.0 * std::pow (1.0 / 8.0, keyPosition);
    decayCoefficient = coefficientForTimeConstant (decaySeconds, sampleRate);
    releaseCoefficient = coefficientForTimeConstant (releaseTimeConstant, sampleRate);
    fadeDecrement = 0.0f;
}

void Voice::release() noexcept
{
    if (currentStage == Stage::Playing)
        currentStage = Stage::Releasing;
    keyDown = false;
    sustained = false;
}

void Voice::fadeOut() noexcept
{
    if (currentStage == Stage::Idle || currentStage == Stage::Fading)
        return;

    currentStage = Stage::Fading;
    keyDown = false;
    sustained = false;
    attacking = false;
    fadeDecrement = std::max (envelope, silenceThreshold) / static_cast<float> (fadeSeconds * sampleRate);
}

void Voice::kill() noexcept
{
    currentStage = Stage::Idle;
    keyDown = false;
    sustained = false;
    envelope = 0.0f;
    midiNote = -1;
}

void Voice::render (float* left, float* right, int numSamples) noexcept
{
    if (currentStage == Stage::Idle)
        return;

    for (int i = 0; i < numSamples; ++i)
    {
        switch (currentStage)
        {
            case Stage::Playing:
            case Stage::Releasing:
                if (attacking)
                {
                    envelope += attackIncrement;
                    if (envelope >= 1.0f) { envelope = 1.0f; attacking = false; }
                }
                else
                {
                    envelope *= currentStage == Stage::Playing ? decayCoefficient : releaseCoefficient;
                }
                break;

            case Stage::Fading:
                envelope -= fadeDecrement;
                break;

            case Stage::Idle:
                break;
        }

        if (envelope <= silenceThreshold && ! attacking)
        {
            kill();
            return;
        }

        // Fundamental plus a quieter second harmonic (sin 2x = 2 sin x cos x).
        const float sample = gain * envelope * (sinPhase + 0.3f * 2.0f * sinPhase * cosPhase);
        if (left != nullptr)  left[i]  += sample;
        if (right != nullptr) right[i] += sample;

        const float nextCos = cosPhase * cosStep - sinPhase * sinStep;
        sinPhase = sinPhase * cosStep + cosPhase * sinStep;
        cosPhase = nextCos;

        // Keep the rotating oscillator on the unit circle despite float rounding.
        // Done on a fixed schedule so the output never depends on the host's buffer size.
        if (++samplesSinceNormalise == 256)
        {
            samplesSinceNormalise = 0;
            const float magnitude = std::sqrt (cosPhase * cosPhase + sinPhase * sinPhase);
            cosPhase /= magnitude;
            sinPhase /= magnitude;
        }
    }
}

} // namespace ck
