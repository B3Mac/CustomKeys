#include "TestFramework.h"

#include "customkeys/Engine.h"

#include <algorithm>
#include <cmath>
#include <vector>

using ck::Engine;
using ck::MidiEvent;

namespace {

constexpr double sampleRate = 48000.0;

struct Stereo
{
    std::vector<float> left, right;
    explicit Stereo (int n) : left (static_cast<size_t> (n)), right (static_cast<size_t> (n)) {}
};

void render (Engine& engine, Stereo& out, int offset, int numSamples, std::vector<MidiEvent> events = {})
{
    float* channels[] = { out.left.data() + offset, out.right.data() + offset };
    engine.process (channels, 2, numSamples, events);
}

Stereo renderBlock (Engine& engine, int numSamples, std::vector<MidiEvent> events = {})
{
    Stereo out (numSamples);
    render (engine, out, 0, numSamples, std::move (events));
    return out;
}

float peak (const std::vector<float>& buffer, size_t from = 0, size_t to = SIZE_MAX)
{
    to = std::min (to, buffer.size());
    float result = 0.0f;
    for (size_t i = from; i < to; ++i)
        result = std::max (result, std::abs (buffer[i]));
    return result;
}

int firstNonZero (const std::vector<float>& buffer)
{
    for (size_t i = 0; i < buffer.size(); ++i)
        if (buffer[i] != 0.0f)
            return static_cast<int> (i);
    return -1;
}

} // namespace

CK_TEST ("silence when nothing is played")
{
    Engine engine;
    engine.prepare (sampleRate);
    auto out = renderBlock (engine, 512);
    CK_CHECK (peak (out.left) == 0.0f);
    CK_CHECK (peak (out.right) == 0.0f);
}

CK_TEST ("note-on starts on the exact sample it was sent")
{
    Engine engine;
    engine.prepare (sampleRate);
    auto out = renderBlock (engine, 256, { MidiEvent::noteOn (100, 60, 100) });

    // The first output sample of a sine starting at phase zero is zero, so sound begins at 101.
    CK_CHECK (peak (out.left, 0, 100) == 0.0f);
    CK_CHECK (firstNonZero (out.left) == 101);
    CK_CHECK (peak (out.left, 100) > 0.0f);
    CK_CHECK (engine.activeVoiceCount() == 1);
}

CK_TEST ("output is identical whatever the buffer size")
{
    const std::vector<MidiEvent> events = { MidiEvent::noteOn (10, 60, 90),
                                            MidiEvent::noteOn (300, 64, 70),
                                            MidiEvent::noteOff (700, 60),
                                            MidiEvent::noteOn (900, 67, 120) };
    Engine big;
    big.prepare (sampleRate);
    auto reference = renderBlock (big, 1024, events);

    Engine small;
    small.prepare (sampleRate);
    Stereo pieces (1024);
    for (int start = 0; start < 1024; start += 64)
    {
        std::vector<MidiEvent> local;
        for (auto e : events)
            if (static_cast<int> (e.sampleOffset) >= start && static_cast<int> (e.sampleOffset) < start + 64)
                local.push_back ({ e.sampleOffset - static_cast<uint32_t> (start), e.status, e.data1, e.data2 });
        render (small, pieces, start, 64, local);
    }

    CK_CHECK (reference.left == pieces.left);
    CK_CHECK (reference.right == pieces.right);
}

CK_TEST ("louder velocity gives a louder note")
{
    Engine soft, loud;
    soft.prepare (sampleRate);
    loud.prepare (sampleRate);
    auto quiet = renderBlock (soft, 2048, { MidiEvent::noteOn (0, 60, 30) });
    auto strong = renderBlock (loud, 2048, { MidiEvent::noteOn (0, 60, 120) });
    CK_CHECK (peak (strong.left) > 4.0f * peak (quiet.left));
}

CK_TEST ("note-off releases to silence and frees the voice")
{
    Engine engine;
    engine.prepare (sampleRate);
    renderBlock (engine, 4800, { MidiEvent::noteOn (0, 60, 100) });
    renderBlock (engine, 480, { MidiEvent::noteOff (0, 60) });
    CK_CHECK (engine.activeVoiceCount() == 1); // still in its release tail

    auto tail = renderBlock (engine, static_cast<int> (sampleRate)); // one second
    CK_CHECK (engine.activeVoiceCount() == 0);
    CK_CHECK (peak (tail.left, tail.left.size() - 512) == 0.0f);
}

CK_TEST ("sustain pedal holds notes until it is lifted")
{
    Engine engine;
    engine.prepare (sampleRate);
    renderBlock (engine, 480, { MidiEvent::sustain (0, true), MidiEvent::noteOn (0, 60, 100) });
    renderBlock (engine, 480, { MidiEvent::noteOff (0, 60) });

    auto held = renderBlock (engine, static_cast<int> (sampleRate));
    CK_CHECK (engine.activeVoiceCount() == 1);
    CK_CHECK (peak (held.left, held.left.size() - 512) > 0.01f);

    renderBlock (engine, 480, { MidiEvent::sustain (0, false) });
    renderBlock (engine, static_cast<int> (sampleRate));
    CK_CHECK (engine.activeVoiceCount() == 0);
}

CK_TEST ("re-striking a note damps the old voice instead of stacking")
{
    Engine engine;
    engine.prepare (sampleRate);
    renderBlock (engine, 480, { MidiEvent::noteOn (0, 60, 100) });
    renderBlock (engine, 480, { MidiEvent::noteOn (470, 60, 100) });
    CK_CHECK (engine.activeVoiceCount() == 2); // old one is still fading

    renderBlock (engine, 480); // 10 ms, longer than the steal fade
    CK_CHECK (engine.activeVoiceCount() == 1);
}

CK_TEST ("polyphony limit steals released notes before held ones")
{
    Engine engine;
    engine.prepare (sampleRate, 4);

    // Notes 60-63 held, then 60 released so it is ringing in its tail.
    renderBlock (engine, 480, { MidiEvent::noteOn (0, 60, 100), MidiEvent::noteOn (0, 61, 100),
                                MidiEvent::noteOn (0, 62, 100), MidiEvent::noteOn (0, 63, 100) });
    renderBlock (engine, 48, { MidiEvent::noteOff (0, 60) });

    // A fifth note must steal 60, not one of the held keys.
    renderBlock (engine, 480, { MidiEvent::noteOn (0, 64, 100) });

    // A second later, 60's release tail would have ended anyway. If a held key had
    // been stolen instead, only three voices would be left.
    renderBlock (engine, static_cast<int> (sampleRate));
    CK_CHECK (engine.activeVoiceCount() == 4);
}

CK_TEST ("a voice is always available, even when every slot is fading")
{
    Engine engine;
    engine.prepare (sampleRate, 2);

    std::vector<MidiEvent> burst;
    for (uint8_t note = 30; note < 90; ++note)
        burst.push_back (MidiEvent::noteOn (0, note, 100));
    renderBlock (engine, 64, burst);

    CK_CHECK (engine.activeVoiceCount() <= 2 + Engine::fadeHeadroom);
    renderBlock (engine, 960);
    CK_CHECK (engine.activeVoiceCount() == 2);
}

CK_TEST ("stealing fades out instead of cutting (no click)")
{
    Engine engine;
    engine.prepare (sampleRate, 1);
    renderBlock (engine, 4800, { MidiEvent::noteOn (0, 48, 127) });

    // Stealing note 48 for note 50: compare the step at the switch with normal sample-to-sample motion.
    auto out = renderBlock (engine, 480, { MidiEvent::noteOn (0, 50, 1) });
    float largestStep = 0.0f;
    for (size_t i = 1; i < out.left.size(); ++i)
        largestStep = std::max (largestStep, std::abs (out.left[i] - out.left[i - 1]));

    CK_CHECK (largestStep < 0.01f);
}

CK_TEST ("all-sound-off silences everything quickly")
{
    Engine engine;
    engine.prepare (sampleRate);
    renderBlock (engine, 480, { MidiEvent::sustain (0, true), MidiEvent::noteOn (0, 60, 100),
                                MidiEvent::noteOn (0, 64, 100) });
    renderBlock (engine, 480, { MidiEvent::controller (0, 120, 0) });
    CK_CHECK (engine.activeVoiceCount() == 0);
    CK_CHECK (! engine.sustainPedalDown());
}

CK_TEST ("audio thread never allocates, even at full polyphony")
{
    Engine engine;
    engine.prepare (sampleRate, 128);

    std::vector<MidiEvent> chord;
    for (uint8_t note = 21; note < 109; ++note)
        chord.push_back (MidiEvent::noteOn (note, note, 100));
    for (uint8_t note = 21; note < 109; ++note)
        chord.push_back (MidiEvent::noteOn (200, note, 80)); // retrigger everything: forces fades and steals

    Stereo out (256);
    float* channels[] = { out.left.data(), out.right.data() };

    cktest::AllocationCounter::start();
    engine.process (channels, 2, 256, chord);
    for (int block = 0; block < 100; ++block)
        engine.process (channels, 2, 256, {});
    const long allocations = cktest::AllocationCounter::stop();

    CK_CHECK (allocations == 0);
}

CK_TEST ("render stats report load and voices")
{
    Engine engine;
    engine.prepare (sampleRate);
    renderBlock (engine, 256, { MidiEvent::noteOn (0, 60, 100), MidiEvent::noteOn (0, 64, 100) });

    const auto stats = engine.stats().snapshot();
    CK_CHECK (stats.activeVoices == 2);
    CK_CHECK (stats.peakVoices == 2);
    CK_CHECK (stats.lastLoad > 0.0f);
    CK_CHECK (stats.overloads == 0);

    engine.stats().resetPeaks();
    CK_CHECK (engine.stats().snapshot().peakVoices == 0);
}

CK_TEST ("mono and multi-channel outputs are handled")
{
    Engine engine;
    engine.prepare (sampleRate);

    std::vector<float> a (256, 1.0f), b (256, 1.0f), c (256, 1.0f);
    float* mono[] = { a.data() };
    std::vector<MidiEvent> on = { MidiEvent::noteOn (0, 60, 100) };
    engine.process (mono, 1, 256, on);
    CK_CHECK (peak (a) > 0.0f && peak (a) < 1.0f);

    float* three[] = { a.data(), b.data(), c.data() };
    engine.process (three, 3, 256, {});
    CK_CHECK (peak (c) == 0.0f); // extra channels are cleared, not left with garbage
}
