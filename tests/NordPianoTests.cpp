// SPDX-License-Identifier: AGPL-3.0-or-later
// Tests for the Nord Piano reader. The files here are synthetic: tones generated
// by the test and encoded the way the piano format stores them (after OpenNord's
// nsp-synth.testing.ts). No Nord audio is used.

#include "TestFramework.h"

#include "customkeys/Engine.h"
#include "customkeys/NordPiano.h"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <vector>

namespace {

struct SynthStroke
{
    std::vector<std::vector<int>> pcm; // per channel
    int floor = 0;                     // as stored in the record
    std::vector<std::array<int, 4>> before; // per channel, oldest first
    int64_t loopStart = 0, loopLength = 0;
    uint32_t loopDecayQ23 = 0;
    int flags = 0;
};

constexpr int pred[5][4] = { { 0, 0, 0, 0 }, { 1, 0, 0, 0 }, { 2, -1, 0, 0 }, { 3, -3, 1, 0 }, { 4, -6, 4, -1 } };

int framesPerBlock (int blockWords, int width, int channels) { return ((blockWords - 1) * 16) / (width * channels) - 64; }

void put16 (std::vector<uint8_t>& b, size_t at, unsigned v) { b[at] = uint8_t (v >> 8); b[at + 1] = uint8_t (v); }
void put32 (std::vector<uint8_t>& b, size_t at, uint32_t v) { put16 (b, at, v >> 16); put16 (b, at + 2, v & 0xffff); }

/** Encodes one stroke into blocks; returns the frame count. */
int64_t encodeStroke (const SynthStroke& s, int channels, int blockWords, std::vector<std::vector<uint8_t>>& blocks)
{
    auto signal = [&] (int c, int64_t n) -> int64_t
    {
        if (n < 0) return s.before[size_t (c)][size_t (4 + n)];
        return n < int64_t (s.pcm[size_t (c)].size()) ? s.pcm[size_t (c)][size_t (n)] : 0;
    };
    const auto total = int64_t (s.pcm[0].size());
    int64_t t = 0;
    for (int b = 0; t < total; ++b)
    {
        const int order = b % 5;
        auto residual = [&] (int c, int64_t n)
        {
            int64_t p = 0;
            for (int j = 0; j < order; ++j) p += pred[order][j] * signal (c, n - 1 - j);
            return signal (c, n) - p;
        };
        int width = 1;
        for (; width <= 20; ++width)
        {
            const int own = framesPerBlock (blockWords, width, channels);
            const int64_t lim = int64_t (1) << (width - 1);
            bool fits = own > 0;
            for (int64_t n = t; fits && n < t + own + 64; ++n)
                for (int c = 0; c < channels; ++c)
                    if (auto r = residual (c, n); r < -lim || r >= lim) { fits = false; break; }
            if (fits) break;
        }
        const int own = framesPerBlock (blockWords, width, channels);
        std::vector<uint16_t> words (size_t (blockWords), 0);
        words[0] = uint16_t (width | (order << 5) | (7 << 8));
        size_t bit = 0;
        for (int64_t n = t; n < t + own + 64; ++n)
            for (int c = 0; c < channels; ++c)
            {
                const auto v = uint64_t ((residual (c, n) + (int64_t (1) << width)) % (int64_t (1) << width));
                for (int k = 0; k < width; ++k, ++bit)
                    if ((v >> k) & 1) words[1 + (bit >> 4)] = uint16_t (words[1 + (bit >> 4)] | (1u << (bit & 15)));
            }
        std::vector<uint8_t> bytes (size_t (blockWords) * 2);
        for (size_t i = 0; i < words.size(); ++i) put16 (bytes, i * 2, words[i]);
        blocks.push_back (std::move (bytes));
        t += own;
    }
    return t;
}

/** A synthetic .npno: keys 0-59 play the zone rooted at 48, keys 60-127 the zone at 72. */
std::vector<uint8_t> makeNpno (const std::vector<SynthStroke>& strokes, int channels, size_t cnsp,
                               const std::array<int, 128>& layersPerRoot)
{
    const int blockWords = 511 * channels;
    std::vector<std::vector<std::vector<uint8_t>>> encoded (strokes.size());
    std::vector<int64_t> frames;
    for (size_t i = 0; i < strokes.size(); ++i)
        frames.push_back (encodeStroke (strokes[i], channels, blockWords, encoded[i]));

    const size_t dataStart = cnsp + 0x732 + strokes.size() * 0x76;
    size_t size = dataStart;
    for (auto& e : encoded) size += e.size() * size_t (blockWords) * 2;
    std::vector<uint8_t> buf (size + (cnsp == 0x18 ? 2 : 0), 0);

    auto ascii = [&] (const char* s, size_t at) { std::memcpy (buf.data() + at, s, std::strlen (s)); };
    ascii ("CBIN", 0);
    if (cnsp == 0x2c) buf[4] = 1;
    ascii ("npno", 8);
    const int version = cnsp == 0x18 ? 530 : 610;
    buf[0x14] = uint8_t (version & 0xff);
    buf[0x15] = uint8_t (version >> 8);
    ascii ("CNSP", cnsp);
    ascii ("Synth Piano#Test", cnsp + 0x1c);
    for (size_t n = 0; n < 128; ++n) buf[cnsp + 0x8c + n] = n < 60 ? 48 : 72;
    put16 (buf, cnsp + 0x61c, cnsp == 0x18 ? 0x450 : 0x464);
    put16 (buf, cnsp + 0x61e, unsigned (channels));
    put16 (buf, cnsp + 0x620, unsigned (strokes.size()));
    for (size_t n = 0; n < 128; ++n) put16 (buf, cnsp + 0x622 + n * 2, unsigned (layersPerRoot[n]));

    size_t at = dataStart;
    for (size_t i = 0; i < strokes.size(); ++i)
    {
        const auto& s = strokes[i];
        const size_t r = cnsp + 0x732 + i * 0x76;
        put32 (buf, r, uint32_t (at - cnsp));
        buf[r + 4] = uint8_t (s.flags);
        buf[r + 5] = uint8_t (s.floor);
        put32 (buf, r + 6, uint32_t (frames[i]));
        put16 (buf, r + 0x0a, unsigned (encoded[i].size()));
        for (size_t c = 0; c < s.before.size(); ++c)
            for (size_t k = 0; k < 4; ++k) put16 (buf, r + 0x0c + c * 8 + k * 2, unsigned (uint16_t (int16_t (s.before[c][k]))));
        put32 (buf, r + 0x1c, uint32_t (s.loopStart));
        put32 (buf, r + 0x20, uint32_t (s.loopLength));
        put32 (buf, r + 0x2e, s.loopDecayQ23);
        for (auto& blk : encoded[i]) { std::memcpy (buf.data() + at, blk.data(), blk.size()); at += blk.size(); }
    }
    return buf;
}

std::vector<int> tone (int frames, double f, double amp, uint32_t seed)
{
    std::vector<int> out (static_cast<size_t> (frames));
    uint32_t s = seed;
    for (int n = 0; n < frames; ++n)
    {
        s = (s * 1103515245u + 12345u) & 0x7fffffffu;
        const double env = std::exp (-n / (frames / 3.0));
        out[size_t (n)] = int (std::lround (amp * env * (std::sin (2 * std::numbers::pi * f * n) + 0.3 * std::sin (4 * std::numbers::pi * f * n))))
                          + int ((s >> 16) % 7) - 3;
    }
    return out;
}

std::array<int, 128> layers (std::initializer_list<std::pair<int, int>> rootsAndCounts)
{
    std::array<int, 128> t {};
    for (auto [root, count] : rootsAndCounts) t[size_t (root)] = count;
    return t;
}

ck::nord::ByteReader readerFor (const std::vector<uint8_t>& bytes)
{
    return [&bytes] (uint64_t offset, size_t count, uint8_t* destination)
    {
        if (offset + count > bytes.size()) return false;
        std::memcpy (destination, bytes.data() + offset, count);
        return true;
    };
}

std::vector<SynthStroke> fourStrokes (int channels)
{
    // Zone at 48: loud (floor 0) and soft (floor 40) layers. Zone at 72: loud and soft.
    std::vector<SynthStroke> s;
    const int floors[] = { 0, 40, 0, 40 };
    for (int i = 0; i < 4; ++i)
    {
        SynthStroke st;
        for (int c = 0; c < channels; ++c)
            st.pcm.push_back (tone (6000 + 1000 * i, 0.01 + 0.005 * i, 9000.0 / (1 + i % 2), uint32_t (i * 2 + c + 1)));
        st.floor = floors[i];
        st.before.assign (size_t (channels), { 0, 3, -2, 5 });
        s.push_back (st);
    }
    s[0].loopStart = 4000;
    s[0].loopLength = 1500;
    s[0].loopDecayQ23 = 8388000; // just below 1.0
    return s;
}

} // namespace

CK_TEST ("nord: decodes a v5-layout mono piano bit-exactly")
{
    auto strokes = fourStrokes (1);
    const auto bytes = makeNpno (strokes, 1, 0x18, layers ({ { 48, 2 }, { 72, 2 } }));

    ck::nord::PianoInfo info;
    std::string error;
    CK_CHECK (ck::nord::readPianoInfo (readerFor (bytes), bytes.size(), info, error));
    CK_CHECK (info.name == "Synth Piano");
    CK_CHECK (info.variant == "Test");
    CK_CHECK (info.version == "5.30");
    CK_CHECK (info.channels == 1);
    CK_CHECK (info.blockWords == 511);
    CK_CHECK (info.zones.size() == 2);
    CK_CHECK (info.zones[0].rootNote == 48 && info.zones[0].lowNote == 0 && info.zones[0].highNote == 59);
    CK_CHECK (info.zones[1].rootNote == 72 && info.zones[1].lowNote == 60 && info.zones[1].highNote == 127);
    CK_CHECK (info.strokes[1].attenuationFloor == 200); // v5 floors are 1 dB steps, scaled to 0.2 dB
    CK_CHECK (info.strokes[0].zone == 0 && info.strokes[1].zone == 0 && info.strokes[2].zone == 1);
    CK_CHECK (info.strokes[0].loopStart == 4000 && info.strokes[0].loopLength == 1500);
    CK_CHECK (info.strokes[0].history[0][0] == 5 && info.strokes[0].history[0][3] == 0); // newest first

    for (size_t i = 0; i < strokes.size(); ++i)
    {
        const auto& s = info.strokes[i];
        std::span<const uint8_t> blocks (bytes.data() + s.offset, size_t (s.blocks) * 511 * 2);
        std::vector<int32_t> pcm;
        CK_CHECK (ck::nord::decodeStroke (blocks, info, s, pcm, error));
        bool same = true;
        for (size_t n = 0; n < pcm.size(); ++n)
            same = same && pcm[n] == (n < strokes[i].pcm[0].size() ? strokes[i].pcm[0][n] : 0);
        CK_CHECK (same);
    }
}

CK_TEST ("nord: decodes a v6-layout stereo piano bit-exactly")
{
    auto strokes = fourStrokes (2);
    const auto bytes = makeNpno (strokes, 2, 0x2c, layers ({ { 48, 2 }, { 72, 2 } }));

    ck::nord::PianoInfo info;
    std::string error;
    CK_CHECK (ck::nord::readPianoInfo (readerFor (bytes), bytes.size(), info, error));
    CK_CHECK (info.version == "6.10");
    CK_CHECK (info.channels == 2);
    CK_CHECK (info.blockWords == 1022);
    CK_CHECK (info.strokes[1].attenuationFloor == 40);

    for (size_t i = 0; i < strokes.size(); ++i)
    {
        const auto& s = info.strokes[i];
        std::span<const uint8_t> blocks (bytes.data() + s.offset, size_t (s.blocks) * 1022 * 2);
        std::vector<int32_t> pcm;
        CK_CHECK (ck::nord::decodeStroke (blocks, info, s, pcm, error));
        bool same = true;
        for (size_t n = 0; n < strokes[i].pcm[0].size(); ++n)
            same = same && pcm[n * 2] == strokes[i].pcm[0][n] && pcm[n * 2 + 1] == strokes[i].pcm[1][n];
        CK_CHECK (same);
    }
}

CK_TEST ("nord: velocity picks layers halfway between their floors")
{
    auto strokes = fourStrokes (1);
    const auto bytes = makeNpno (strokes, 1, 0x2c, layers ({ { 48, 2 }, { 72, 2 } }));
    ck::nord::PianoInfo info;
    std::string error;
    CK_CHECK (ck::nord::readPianoInfo (readerFor (bytes), bytes.size(), info, error));

    // Floors 0 and 40 (0.2 dB steps): the soft layer starts at attenuation 20 (4 dB),
    // reached below velocity 127 * 10^(-0.1) = 100.9.
    CK_CHECK (ck::nord::strokeForVelocity (info, 0, 127) == 0);
    CK_CHECK (ck::nord::strokeForVelocity (info, 0, 101) == 0);
    CK_CHECK (ck::nord::strokeForVelocity (info, 0, 100) == 1);
    CK_CHECK (ck::nord::strokeForVelocity (info, 0, 1) == 1);
    CK_CHECK (ck::nord::strokeForVelocity (info, 1, 127) == 2);
}

CK_TEST ("nord: a damaged or foreign file is refused with a message")
{
    std::string error;
    std::vector<uint8_t> junk (4096, 0x41);
    CK_CHECK (ck::nord::loadPiano (std::span<const uint8_t> (junk), error) == nullptr);
    CK_CHECK (! error.empty());

    auto bytes = makeNpno (fourStrokes (1), 1, 0x2c, layers ({ { 48, 2 }, { 72, 2 } }));
    bytes.resize (bytes.size() - 3000); // truncated
    error.clear();
    CK_CHECK (ck::nord::loadPiano (std::span<const uint8_t> (bytes), error) == nullptr);
    CK_CHECK (! error.empty());
}

CK_TEST ("nord: loads a playable instrument from a file on disk")
{
    const auto bytes = makeNpno (fourStrokes (2), 2, 0x2c, layers ({ { 48, 2 }, { 72, 2 } }));
    const auto path = std::filesystem::temp_directory_path() / "customkeys-synthetic-test.npno";
    {
        std::ofstream out (path, std::ios::binary);
        out.write (reinterpret_cast<const char*> (bytes.data()), std::streamsize (bytes.size()));
    }

    std::atomic<float> progress { 0.0f };
    std::string error;
    auto instrument = ck::nord::loadPiano (path, error, { &progress, nullptr, 3 });
    std::filesystem::remove (path);

    CK_CHECK (instrument != nullptr);
    if (instrument == nullptr) { std::printf ("    %s\n", error.c_str()); return; }

    CK_CHECK (instrument->name == "Synth Piano");
    CK_CHECK (progress.load() == 1.0f);
    CK_CHECK (instrument->samples.size() == 4);
    CK_CHECK (instrument->zoneForKey[40] == 0 && instrument->zoneForKey[80] == 1);
    CK_CHECK (instrument->zones[0].rootKey == 52.0); // root + 4 semitones
    CK_CHECK (instrument->velocityGain[127] == 1.0f);
    CK_CHECK (instrument->velocityGain[64] < 0.6f);

    const auto& loud = instrument->samples[size_t (instrument->zones[0].sampleForVelocity[127])];
    CK_CHECK (loud.channels == 2);
    CK_CHECK (loud.loops());
    CK_CHECK (loud.loopDecayPerSecond < 0.0);

    // It plays: a note on the loud layer makes sound, and the pedal holds it in its loop.
    ck::Engine engine;
    engine.prepare (48000.0);
    engine.setInstrument (instrument);
    std::vector<float> l (4096), r (4096);
    float* out[] = { l.data(), r.data() };
    std::vector<ck::MidiEvent> on = { ck::MidiEvent::sustain (0, true), ck::MidiEvent::noteOn (0, 52, 127),
                                      ck::MidiEvent::noteOff (100, 52) };
    engine.process (out, 2, 4096, on);
    float peak = 0.0f;
    for (float v : l) peak = std::max (peak, std::abs (v));
    CK_CHECK (peak > 0.1f && peak <= 0.36f); // full scale times the mix headroom
    for (int i = 0; i < 20; ++i)
        engine.process (out, 2, 4096, {}); // well past the recording's end
    CK_CHECK (engine.activeVoiceCount() == 1);
}
