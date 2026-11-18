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
    sample = nullptr;
}

void Voice::startSample (int note, const Sample& s, double pitchSemitones, float noteGain,
                         float releaseSeconds, double rate, uint64_t startOrder) noexcept
{
    sampleRate = rate;
    midiNote = note;
    order = startOrder;
    keyDown = true;
    sustained = false;
    currentStage = Stage::Playing;

    sample = &s;
    position = 0.0;
    increment = std::pow (2.0, pitchSemitones / 12.0) * s.sampleRate / rate;

    // The recording carries its own attack and decay; the envelope only shapes
    // the loop's sustain decay, the key-up fade and steals.
    gain = noteGain * s.gain;
    envelope = 1.0f;
    attacking = false;
    loopDecayCoefficient = static_cast<float> (std::exp (std::min (s.loopDecayPerSecond, 0.0) / rate));
    releaseCoefficient = coefficientForTimeConstant (std::max (releaseSeconds, 0.001f), rate);
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
    sample = nullptr;
}

float Voice::readFrame (int64_t frame, int channel) const noexcept
{
    const auto& s = *sample;
    if (frame >= s.loopStart + s.loopLength && s.loops())
        frame -= s.loopLength;
    if (frame < 0)
        frame = 0;
    if (frame >= s.frames)
        return 0.0f;
    return static_cast<float> (s.data[static_cast<size_t> (frame * s.channels + channel)]);
}

void Voice::renderSample (float* left, float* right, int numSamples) noexcept
{
    const auto& s = *sample;
    const bool loops = s.loops();
    const auto loopStart = static_cast<double> (s.loopStart);
    const auto loopEnd = static_cast<double> (s.loopStart + s.loopLength);
    const auto end = static_cast<double> (s.frames);
    const int rightChannel = s.channels > 1 ? 1 : 0;

    for (int i = 0; i < numSamples; ++i)
    {
        switch (currentStage)
        {
            case Stage::Playing:
                if (loops && position >= loopStart)
                    envelope *= loopDecayCoefficient;
                break;
            case Stage::Releasing: envelope *= releaseCoefficient; break;
            case Stage::Fading:    envelope -= fadeDecrement; break;
            case Stage::Idle:      break;
        }

        if (envelope <= silenceThreshold)
        {
            kill();
            return;
        }

        // Four-point cubic Hermite interpolation between recorded frames.
        const auto index = static_cast<int64_t> (position);
        const auto t = static_cast<float> (position - static_cast<double> (index));
        const float level = gain * envelope;

        auto interpolate = [&] (int channel)
        {
            const float y0 = readFrame (index - 1, channel), y1 = readFrame (index, channel);
            const float y2 = readFrame (index + 1, channel), y3 = readFrame (index + 2, channel);
            const float c1 = 0.5f * (y2 - y0);
            const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
            const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
            return ((c3 * t + c2) * t + c1) * t + y1;
        };

        const float l = interpolate (0) * level;
        const float r = rightChannel == 0 ? l : interpolate (rightChannel) * level;
        if (left != nullptr)  left[i]  += l;
        if (right != nullptr) right[i] += r;

        position += increment;
        if (loops && position >= loopEnd)
            position -= static_cast<double> (s.loopLength);
        else if (position >= end)
        {
            kill();
            return;
        }
    }
}

void Voice::render (float* left, float* right, int numSamples) noexcept
{
    if (currentStage == Stage::Idle)
        return;

    if (sample != nullptr)
    {
        renderSample (left, right, numSamples);
        return;
    }

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
        const float tone = gain * envelope * (sinPhase + 0.3f * 2.0f * sinPhase * cosPhase);
        if (left != nullptr)  left[i]  += tone;
        if (right != nullptr) right[i] += tone;

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
