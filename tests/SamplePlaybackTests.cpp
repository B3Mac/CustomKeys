#include "TestFramework.h"

#include "customkeys/Engine.h"

#include <cmath>
#include <memory>
#include <numbers>
#include <vector>

using ck::Engine;
using ck::MidiEvent;

namespace {

constexpr double rate = 48000.0;

/** One zone over every key, rooted at A4, with a loud and a soft sine layer. */
std::shared_ptr<ck::Instrument> sineInstrument (double frequency, int frames, int64_t loopStart = 0, int64_t loopLength = 0)
{
    auto inst = std::make_shared<ck::Instrument>();
    inst->name = "Sine";
    for (int layer = 0; layer < 2; ++layer)
    {
        ck::Sample s;
        s.channels = 1;
        s.frames = frames;
        s.sampleRate = rate;
        s.gain = 1.0f / 32767.0f;
        s.loopStart = loopStart;
        s.loopLength = loopLength;
        s.data.resize (size_t (frames));
        const double f = layer == 0 ? frequency : frequency * 2.0; // the soft layer is an octave up, to tell them apart
        for (int n = 0; n < frames; ++n)
            s.data[size_t (n)] = int16_t (std::lround (32767.0 * std::sin (2.0 * std::numbers::pi * f * n / rate)));
        inst->samples.push_back (std::move (s));
    }
    ck::Zone z;
    z.rootKey = 69.0;
    for (int v = 0; v < 128; ++v)
        z.sampleForVelocity[size_t (v)] = v >= 64 ? 0 : 1;
    inst->zones.push_back (z);
    inst->zoneForKey.fill (0);
    inst->zoneForKey[0] = -1; // a key with no sound
    inst->velocityGain.fill (1.0f);
    inst->outputGain = 0.5f;
    return inst;
}

std::vector<float> renderLeft (Engine& engine, int n, std::vector<MidiEvent> events = {})
{
    std::vector<float> l (static_cast<size_t> (n)), r (static_cast<size_t> (n));
    float* out[] = { l.data(), r.data() };
    engine.process (out, 2, n, events);
    return l;
}

/** Frequency from zero crossings over the buffer. */
double measureFrequency (const std::vector<float>& x, size_t from)
{
    int crossings = 0;
    size_t first = 0, last = 0;
    for (size_t i = from + 1; i < x.size(); ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
        {
            if (crossings == 0) first = i;
            last = i;
            ++crossings;
        }
    return crossings > 1 ? (crossings - 1) * rate / double (last - first) : 0.0;
}

float peakOf (const std::vector<float>& x, size_t from = 0)
{
    float p = 0.0f;
    for (size_t i = from; i < x.size(); ++i) p = std::max (p, std::abs (x[i]));
    return p;
}

} // namespace

CK_TEST ("samples: the root key plays the recording at its own pitch")
{
    Engine engine;
    engine.prepare (rate);
    engine.setInstrument (sineInstrument (440.0, 48000));
    const auto out = renderLeft (engine, 9600, { MidiEvent::noteOn (0, 69, 100) });
    CK_CHECK (std::abs (measureFrequency (out, 0) - 440.0) < 1.0);
    CK_CHECK (std::abs (peakOf (out) - 0.5f) < 0.01f);
}

CK_TEST ("samples: other keys are transposed by equal temperament")
{
    Engine engine;
    engine.prepare (rate);
    engine.setInstrument (sineInstrument (440.0, 48000));
    const auto out = renderLeft (engine, 9600, { MidiEvent::noteOn (0, 76, 100) }); // a fifth up
    CK_CHECK (std::abs (measureFrequency (out, 0) - 440.0 * std::pow (2.0, 7.0 / 12.0)) < 1.5);
}

CK_TEST ("samples: velocity chooses the layer")
{
    Engine engine;
    engine.prepare (rate);
    engine.setInstrument (sineInstrument (440.0, 48000));
    const auto soft = renderLeft (engine, 9600, { MidiEvent::noteOn (0, 69, 40) });
    CK_CHECK (std::abs (measureFrequency (soft, 0) - 880.0) < 2.0);
}

CK_TEST ("samples: a key the instrument doesn't cover stays silent")
{
    Engine engine;
    engine.prepare (rate);
    engine.setInstrument (sineInstrument (440.0, 48000));
    const auto out = renderLeft (engine, 512, { MidiEvent::noteOn (0, 0, 100) });
    CK_CHECK (peakOf (out) == 0.0f);
    CK_CHECK (engine.activeVoiceCount() == 0);
}

CK_TEST ("samples: a note without a loop ends with its recording")
{
    Engine engine;
    engine.prepare (rate);
    engine.setInstrument (sineInstrument (440.0, 4800));
    renderLeft (engine, 4000, { MidiEvent::noteOn (0, 69, 100) });
    CK_CHECK (engine.activeVoiceCount() == 1);
    renderLeft (engine, 1000);
    CK_CHECK (engine.activeVoiceCount() == 0);
}

CK_TEST ("samples: a looping note sustains while held and releases on key-up")
{
    Engine engine;
    engine.prepare (rate);
    // 4800 frames with a loop over the last 2400 (a whole number of 440 Hz... close enough for the test).
    engine.setInstrument (sineInstrument (440.0, 4800, 2400, 2400));
    renderLeft (engine, 512, { MidiEvent::noteOn (0, 69, 100) });
    const auto held = renderLeft (engine, 48000); // a second, far past the recording
    CK_CHECK (engine.activeVoiceCount() == 1);
    CK_CHECK (peakOf (held, 47000) > 0.4f);

    renderLeft (engine, 512, { MidiEvent::noteOff (0, 69) });
    renderLeft (engine, 48000);
    CK_CHECK (engine.activeVoiceCount() == 0);
}

CK_TEST ("samples: the sustain pedal holds a looping note after key-up")
{
    Engine engine;
    engine.prepare (rate);
    engine.setInstrument (sineInstrument (440.0, 4800, 2400, 2400));
    renderLeft (engine, 512, { MidiEvent::sustain (0, true), MidiEvent::noteOn (0, 69, 100), MidiEvent::noteOff (100, 69) });
    const auto held = renderLeft (engine, 24000);
    CK_CHECK (engine.activeVoiceCount() == 1);
    CK_CHECK (peakOf (held, 23000) > 0.4f);
    renderLeft (engine, 512, { MidiEvent::sustain (0, false) });
    renderLeft (engine, 48000);
    CK_CHECK (engine.activeVoiceCount() == 0);
}

CK_TEST ("samples: loop decay makes a held note fade")
{
    Engine engine;
    engine.prepare (rate);
    auto inst = sineInstrument (440.0, 4800, 2400, 2400);
    for (auto& s : inst->samples) s.loopDecayPerSecond = std::log (0.1); // -20 dB a second
    engine.setInstrument (inst);
    renderLeft (engine, 4800, { MidiEvent::noteOn (0, 69, 100) });
    const auto later = renderLeft (engine, 48000);
    CK_CHECK (peakOf (later, 47000) < 0.07f);
    CK_CHECK (peakOf (later, 47000) > 0.03f);
}

CK_TEST ("samples: swapping instruments silences old notes and frees the old one")
{
    Engine engine;
    engine.prepare (rate);
    auto first = sineInstrument (440.0, 48000);
    std::weak_ptr<const ck::Instrument> watch = first;
    engine.setInstrument (std::move (first));
    renderLeft (engine, 256, { MidiEvent::noteOn (0, 69, 100) });
    CK_CHECK (engine.activeVoiceCount() == 1);

    engine.setInstrument (sineInstrument (220.0, 48000));
    CK_CHECK (! watch.expired()); // the audio thread hasn't let go yet
    renderLeft (engine, 256);
    CK_CHECK (engine.activeVoiceCount() == 0);
    engine.collectGarbage();
    CK_CHECK (watch.expired());

    engine.setInstrument (nullptr); // back to the test tone
    renderLeft (engine, 256);
    renderLeft (engine, 256, { MidiEvent::noteOn (0, 69, 100) });
    CK_CHECK (engine.activeVoiceCount() == 1);
}

CK_TEST ("samples: the audio thread never allocates while playing samples")
{
    Engine engine;
    engine.prepare (rate, 128);
    engine.setInstrument (sineInstrument (440.0, 48000, 24000, 24000));

    std::vector<MidiEvent> chord;
    for (uint8_t note = 21; note < 109; ++note)
        chord.push_back (MidiEvent::noteOn (note, note, 100));
    for (uint8_t note = 21; note < 109; ++note)
        chord.push_back (MidiEvent::noteOn (200, note, 30));

    std::vector<float> l (256), r (256);
    float* out[] = { l.data(), r.data() };
    cktest::AllocationCounter::start();
    engine.process (out, 2, 256, chord);
    for (int block = 0; block < 100; ++block)
        engine.process (out, 2, 256, {});
    CK_CHECK (cktest::AllocationCounter::stop() == 0);
}

CK_TEST ("samples: a huge pedalled chord is limited instead of clipping")
{
    Engine engine;
    engine.prepare (rate);
    engine.setInstrument (sineInstrument (440.0, 48000, 24000, 24000));

    std::vector<MidiEvent> chord { MidiEvent::sustain (0, true) };
    for (uint8_t note = 40; note < 100; note += 2)
        chord.push_back (MidiEvent::noteOn (0, note, 127)); // 30 notes at 0.5 each
    auto out = renderLeft (engine, 48000, chord);

    CK_CHECK (peakOf (out) <= 0.891f);
    CK_CHECK (peakOf (out) > 0.5f);
    CK_CHECK (engine.stats().snapshot().limitedBlocks > 0);

    // A single quiet note afterwards plays at its own level again once the limiter recovers.
    Engine quiet;
    quiet.prepare (rate);
    quiet.setInstrument (sineInstrument (440.0, 48000));
    auto one = renderLeft (quiet, 4800, { MidiEvent::noteOn (0, 69, 100) });
    CK_CHECK (std::abs (peakOf (one) - 0.5f) < 0.01f);
    CK_CHECK (quiet.stats().snapshot().limitedBlocks == 0);
}

CK_TEST ("samples: steals are counted")
{
    Engine engine;
    engine.prepare (rate, 4);
    engine.setInstrument (sineInstrument (440.0, 48000));
    std::vector<MidiEvent> notes;
    for (uint8_t note = 60; note < 66; ++note)
        notes.push_back (MidiEvent::noteOn (0, note, 100));
    renderLeft (engine, 256, notes);
    CK_CHECK (engine.stats().snapshot().steals == 2);
}
