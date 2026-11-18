#include "customkeys/Engine.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <tuple>

namespace ck {

Engine::Engine()
{
    prepare (sampleRate, defaultPolyphony);
}

void Engine::prepare (double newSampleRate, int maxPolyphony)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    polyphony = std::clamp (maxPolyphony, 1, maxPolyphonyLimit);
    voices.assign (static_cast<size_t> (polyphony + fadeHeadroom), Voice {});
    sustainDown = false;
    noteCounter = 0;
}

void Engine::reset() noexcept
{
    for (auto& voice : voices)
        voice.kill();
    sustainDown = false;
}

int Engine::activeVoiceCount() const noexcept
{
    return static_cast<int> (std::count_if (voices.begin(), voices.end(),
                                            [] (const Voice& v) { return ! v.isIdle(); }));
}

void Engine::process (float* const* outputs, int numChannels, int numSamples,
                      std::span<const MidiEvent> events) noexcept
{
    const auto startTime = std::chrono::steady_clock::now();

    for (int ch = 0; ch < numChannels; ++ch)
        if (outputs[ch] != nullptr)
            std::memset (outputs[ch], 0, sizeof (float) * static_cast<size_t> (std::max (numSamples, 0)));

    float* left  = numChannels > 0 ? outputs[0] : nullptr;
    float* right = numChannels > 1 ? outputs[1] : nullptr;

    // Render up to each event's sample position, then apply it, so timing is
    // sample-accurate no matter how large the host's buffer is.
    int position = 0;
    auto renderUntil = [&] (int end)
    {
        if (end <= position)
            return;
        const int length = end - position;
        for (auto& voice : voices)
            voice.render (left != nullptr ? left + position : nullptr,
                          right != nullptr ? right + position : nullptr,
                          length);
        position = end;
    };

    for (const auto& event : events)
    {
        renderUntil (std::min (static_cast<int> (event.sampleOffset), numSamples));
        handleEvent (event);
    }
    renderUntil (numSamples);

    if (numSamples > 0)
    {
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - startTime;
        const double blockSeconds = numSamples / sampleRate;
        renderStats.recordBlock (static_cast<float> (elapsed.count() / blockSeconds), activeVoiceCount());
    }
}

void Engine::handleEvent (const MidiEvent& event) noexcept
{
    const int type = event.status & 0xF0;

    if (type == 0x90 && event.data2 > 0)
        noteOn (event.data1 & 0x7F, event.data2 & 0x7F);
    else if (type == 0x80 || type == 0x90)
        noteOff (event.data1 & 0x7F);
    else if (type == 0xB0)
    {
        switch (event.data1)
        {
            case 64:  setSustain (event.data2 >= 64); break;
            case 120: fadeAll(); break;     // All Sound Off
            case 123: releaseAll(); break;  // All Notes Off
            default: break;
        }
    }
}

void Engine::noteOn (int note, int velocity) noexcept
{
    // Re-striking a note damps the voice already playing it.
    for (auto& voice : voices)
        if (! voice.isIdle() && ! voice.isFading() && voice.note() == note)
            voice.fadeOut();

    if (countSoundingVoices() >= polyphony)
        if (auto* victim = chooseVoiceToSteal())
            victim->fadeOut();

    Voice* voice = findFreeVoice();

    if (voice == nullptr)
    {
        // Every slot, headroom included, is busy. Reuse the quietest fading voice;
        // sounding voices never exceed the polyphony limit, so one always exists.
        for (auto& candidate : voices)
            if (candidate.isFading() && (voice == nullptr || candidate.level() < voice->level()))
                voice = &candidate;

        if (voice == nullptr)
            return;

        voice->kill();
    }

    voice->start (note, static_cast<float> (velocity) / 127.0f, sampleRate, ++noteCounter);
}

void Engine::noteOff (int note) noexcept
{
    for (auto& voice : voices)
    {
        if (voice.isIdle() || voice.isFading() || voice.note() != note || ! voice.keyDown)
            continue;

        if (sustainDown)
        {
            voice.keyDown = false;
            voice.sustained = true;
        }
        else
        {
            voice.release();
        }
    }
}

void Engine::setSustain (bool down) noexcept
{
    sustainDown = down;

    if (! down)
        for (auto& voice : voices)
            if (voice.sustained)
                voice.release();
}

void Engine::releaseAll() noexcept
{
    for (auto& voice : voices)
        voice.release();
}

void Engine::fadeAll() noexcept
{
    for (auto& voice : voices)
        voice.fadeOut();
    sustainDown = false;
}

Voice* Engine::findFreeVoice() noexcept
{
    for (auto& voice : voices)
        if (voice.isIdle())
            return &voice;
    return nullptr;
}

int Engine::countSoundingVoices() const noexcept
{
    return static_cast<int> (std::count_if (voices.begin(), voices.end(),
                                            [] (const Voice& v) { return ! v.isIdle() && ! v.isFading(); }));
}

Voice* Engine::chooseVoiceToSteal() noexcept
{
    // Released voices first, then notes held only by the pedal, then held keys.
    // Within a group, the quietest goes first, and the oldest breaks ties.
    auto priority = [] (const Voice& v)
    {
        const int group = v.keyDown ? 2 : (v.sustained ? 1 : 0);
        return std::make_tuple (group, v.level(), v.startOrder());
    };

    Voice* best = nullptr;
    for (auto& voice : voices)
    {
        if (voice.isIdle() || voice.isFading())
            continue;
        if (best == nullptr || priority (voice) < priority (*best))
            best = &voice;
    }
    return best;
}

} // namespace ck
