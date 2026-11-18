// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Nord Piano Library (.npno) reader, ported to C++ from OpenNord
// (https://github.com/simonflore/opennord: src/lib/ns4/nsp.ts, nsp-audio.ts and
// src/components/piano/pianoEngine.ts), Copyright the OpenNord contributors,
// licensed under the GNU AGPL v3. See NOTICE.md next to this file.
//
// Layout (offsets from the `CNSP` tag, 0x2C in v6 files and 0x18 in v5; big-endian
// inside the CNSP container):
//   +0x16   metadata block, name as "name#variant"
//   +0x8C   128-byte per-key map: each byte is the root note of that key's zone
//   +0x10C, +0x38C  per-key level trims (int8); +0x40C a global trim
//   +0x18C  per-key tuning correction (int8 cents; added in v6, subtracted in v5)
//   +0x40D  first key without a damper; +0x49C release mode
//   +0x61C  u16 version, u16 channels, u16 stroke count, then u16[128] layers per root
//   +0x732  stroke records, 0x76 bytes each
//   blocks  511 words mono / 1022 stereo. Word 0: bits 0-4 residual width, 5-7
//           predictor order (0-4). Then residuals, LSB-first over 16-bit words, L/R
//           interleaved. The last 64 frames of a block repeat the next block's first 64.
//
// The format is a lossless predictive codec; nothing here involves encryption.

#include "customkeys/NordPiano.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>

namespace ck::nord {

namespace {

constexpr size_t cnspSearchLimit = 0x80;
constexpr size_t metaOffset = 0x16;
constexpr size_t metaSize = 0x67;
constexpr size_t keymapOffset = 0x8c;
constexpr size_t dirOffset = 0x61c;
constexpr size_t layerTableOffset = 0x622;
constexpr size_t recordsOffset = 0x732;
constexpr size_t recordSize = 0x76;
constexpr int blockOverlap = 64;
constexpr int cnspV6 = 0x464;
constexpr size_t keyTrimOffsetA = 0x10c;
constexpr size_t keyTuneOffset = 0x18c;
constexpr size_t keyTrimOffsetB = 0x38c;
constexpr size_t globalTrimOffset = 0x40c;
constexpr size_t undampedFromOffset = 0x40d;
constexpr size_t releaseModeOffset = 0x49c;

constexpr int predictors[5][4] = { { 0, 0, 0, 0 }, { 1, 0, 0, 0 }, { 2, -1, 0, 0 }, { 3, -3, 1, 0 }, { 4, -6, 4, -1 } };

uint16_t be16 (const uint8_t* p) noexcept { return static_cast<uint16_t> ((p[0] << 8) | p[1]); }
uint32_t be32 (const uint8_t* p) noexcept { return (uint32_t { be16 (p) } << 16) | be16 (p + 2); }
int16_t be16s (const uint8_t* p) noexcept { return static_cast<int16_t> (be16 (p)); }
int s8 (uint8_t b) noexcept { return b > 127 ? b - 256 : b; }

int findCnsp (std::span<const uint8_t> bytes) noexcept
{
    const size_t limit = std::min (bytes.size(), cnspSearchLimit);
    for (size_t i = 0; i + 4 <= limit; ++i)
        if (std::memcmp (bytes.data() + i, "CNSP", 4) == 0)
            return static_cast<int> (i);
    return -1;
}

std::string trim (const std::string& s)
{
    const auto b = s.find_first_not_of (' ');
    const auto e = s.find_last_not_of (' ');
    return b == std::string::npos ? std::string() : s.substr (b, e - b + 1);
}

std::string firstAsciiRun (std::span<const uint8_t> bytes, size_t from, size_t to, size_t minLength = 3)
{
    std::string run;
    for (size_t i = from; i < std::min (to, bytes.size()); ++i)
    {
        const auto c = bytes[i];
        if (c >= 0x20 && c < 0x7f)
            run += static_cast<char> (c);
        else
        {
            if (run.size() >= minLength)
                return trim (run);
            run.clear();
        }
    }
    return run.size() >= minLength ? trim (run) : std::string();
}

int framesPerBlock (int blockWords, int width, int channels) noexcept
{
    return ((blockWords - 1) * 16) / (width * channels) - blockOverlap;
}

int64_t maxFrames (int blocks, int blockWords, int channels) noexcept
{
    return int64_t { blocks } * framesPerBlock (blockWords, 1, channels);
}

} // namespace

int PianoInfo::strokeZoneCount() const noexcept
{
    int count = 0;
    for (const auto& s : strokes)
        count = std::max (count, s.zone + 1);
    return count;
}

bool looksLikePiano (std::span<const uint8_t> b) noexcept
{
    return b.size() >= 12 && std::memcmp (b.data(), "CBIN", 4) == 0 && std::memcmp (b.data() + 8, "npno", 4) == 0;
}

bool readPianoInfo (const ByteReader& read, uint64_t fileSize, PianoInfo& info, std::string& error)
{
    info = {};

    std::vector<uint8_t> head (static_cast<size_t> (std::min<uint64_t> (fileSize, cnspSearchLimit)));
    if (head.size() < 0x18 || ! read (0, head.size(), head.data()) || ! looksLikePiano (head))
    {
        error = "This isn't a Nord Piano Library file (.npno).";
        return false;
    }

    const int cnsp = findCnsp (head);
    if (cnsp < 0 || cnsp + recordsOffset > fileSize)
    {
        error = "The piano file has no readable directory; it may be damaged or an unsupported version.";
        return false;
    }

    const int versionRaw = head[0x14] | (head[0x15] << 8);
    const int major = versionRaw / 100;
    const int minor = versionRaw - major * 100;
    info.version = std::to_string (major) + "." + (minor < 10 ? "0" : "") + std::to_string (minor);

    // Header through the fixed directory, then the stroke records once their count is known.
    std::vector<uint8_t> header (static_cast<size_t> (cnsp) + recordsOffset);
    if (! read (0, header.size(), header.data()))
    {
        error = "Couldn't read the piano file.";
        return false;
    }

    const uint8_t* base = header.data() + cnsp;
    const int dirVersion = be16 (base + dirOffset);
    info.channels = be16 (base + dirOffset + 2);
    const int count = be16 (base + dirOffset + 4);

    if ((info.channels != 1 && info.channels != 2) || count == 0
        || static_cast<uint64_t> (cnsp) + recordsOffset + uint64_t (count) * recordSize > fileSize)
    {
        error = "The piano file's directory is unreadable; it may be damaged or an unsupported version.";
        return false;
    }

    header.resize (static_cast<size_t> (cnsp) + recordsOffset + static_cast<size_t> (count) * recordSize);
    if (! read (0, header.size(), header.data()))
    {
        error = "Couldn't read the piano file.";
        return false;
    }
    base = header.data() + cnsp;

    // Name: "name#variant", space-padded.
    const auto full = firstAsciiRun (header, static_cast<size_t> (cnsp) + metaOffset, static_cast<size_t> (cnsp) + metaOffset + metaSize);
    if (const auto hash = full.find ('#'); hash != std::string::npos)
    {
        info.name = trim (full.substr (0, hash));
        info.variant = trim (full.substr (hash + 1));
    }
    else
        info.name = full;

    std::array<int, 128> layersPerRoot {};
    for (size_t n = 0; n < 128; ++n)
        layersPerRoot[n] = be16 (base + layerTableOffset + n * 2);

    // Key map: each byte is the root note of the zone that plays the key; 0 and 0xFF are unused.
    std::map<int, std::pair<int, int>> span;
    for (int note = 0; note < 128; ++note)
    {
        const int root = base[keymapOffset + static_cast<size_t> (note)];
        if (root == 0 || root > 127)
            continue;
        auto [it, inserted] = span.try_emplace (root, note, note);
        if (! inserted)
        {
            it->second.first = std::min (it->second.first, note);
            it->second.second = std::max (it->second.second, note);
        }
    }

    // Any byte run can look like one root, so a single zone counts only when the
    // layer table agrees: strokes at that root and nowhere else.
    bool keymapOk = ! span.empty();
    if (span.size() == 1)
        for (int n = 0; n < 128; ++n)
            if ((n == span.begin()->first) != (layersPerRoot[static_cast<size_t> (n)] > 0))
                keymapOk = false;

    if (keymapOk)
        for (const auto& [root, range] : span)
            info.zones.push_back ({ root, range.first, range.second });
    std::sort (info.zones.begin(), info.zones.end(), [] (const auto& a, const auto& b) { return a.lowNote < b.lowNote; });

    // Per-key tuning, level trims and dampers (header, relative to CNSP).
    info.v6 = dirVersion >= cnspV6;
    for (size_t k = 0; k < 128; ++k)
    {
        info.keyTuneCents[k] = (info.v6 ? 1 : -1) * s8 (base[keyTuneOffset + k]);
        info.keyLevelTrim[k] = s8 (base[keyTrimOffsetA + k]) + s8 (base[keyTrimOffsetB + k]) + s8 (base[globalTrimOffset]);
    }
    info.undampedFrom = base[undampedFromOffset];
    if (info.undampedFrom < 60) // 0 from old tools; factory pianos store 89 or above. Every key damped is the safe reading.
        info.undampedFrom = 128;
    info.releaseMode = base[releaseModeOffset];

    // Stroke records.
    const int floorScale = dirVersion < cnspV6 ? 5 : 1; // v5 stores floors in 1 dB steps
    info.strokes.resize (static_cast<size_t> (count));
    for (int i = 0; i < count; ++i)
    {
        const uint8_t* r = base + recordsOffset + static_cast<size_t> (i) * recordSize;
        auto& s = info.strokes[static_cast<size_t> (i)];
        s.index = i;
        s.offset = static_cast<uint64_t> (cnsp) + be32 (r);
        s.flags = r[0x04] & 3;
        s.attenuationFloor = r[0x05] * floorScale;
        s.frames = be32 (r + 0x06);
        s.blocks = be16 (r + 0x0a);
        for (int c = 0; c < info.channels; ++c)
            for (int k = 0; k < 4; ++k) // stored oldest first
                s.history[static_cast<size_t> (c)][static_cast<size_t> (3 - k)] = be16s (r + (c == 0 ? 0x0c : 0x14) + k * 2);
        s.loopStart = be32 (r + 0x1c);
        s.loopLength = be32 (r + 0x20);
        s.loopDecay = s.loopLength > 0 ? be32 (r + 0x2e) / 8388608.0 : 1.0;
        s.fineTune = s8 (r[0x32]);
        s.levelWord = be16 (r + 0x34);
        s.damperIndex = r[0x72];
    }

    // Zone of each stroke: from the layer table, or "a loudest normal layer opens a zone".
    int tableTotal = 0;
    std::vector<int> counts;
    for (int c : layersPerRoot)
        if (c > 0) { counts.push_back (c); tableTotal += c; }

    if (tableTotal == count)
    {
        size_t at = 0;
        for (size_t z = 0; z < counts.size(); ++z)
            for (int k = 0; k < counts[z]; ++k)
                info.strokes[at++].zone = static_cast<int> (z);
    }
    else
    {
        int zone = -1;
        for (auto& s : info.strokes)
        {
            if ((s.attenuationFloor == 0 && s.flags != 2) || zone < 0)
                ++zone;
            s.zone = zone;
        }
    }

    // Strokes are stored back to back, so the block size falls out of two neighbours.
    info.blockWords = 511 * info.channels;
    if (info.strokes.size() > 1 && info.strokes[0].blocks > 0)
    {
        const auto gap = static_cast<int64_t> (info.strokes[1].offset) - static_cast<int64_t> (info.strokes[0].offset);
        const auto divisor = int64_t { info.strokes[0].blocks } * 2;
        if (gap <= 0 || gap % divisor != 0 || gap / divisor < 2 || gap / divisor > 65535)
        {
            error = "The piano file's block layout is unreadable.";
            return false;
        }
        info.blockWords = static_cast<int> (gap / divisor);
    }

    for (const auto& s : info.strokes)
    {
        if (s.offset + uint64_t (s.blocks) * uint64_t (info.blockWords) * 2 > fileSize
            || s.frames > maxFrames (s.blocks, info.blockWords, info.channels))
        {
            error = "The piano file is truncated or damaged (stroke " + std::to_string (s.index) + ").";
            return false;
        }
    }

    return true;
}

bool decodeStroke (std::span<const uint8_t> bytes, const PianoInfo& info, const Stroke& stroke,
                   std::vector<int32_t>& out, std::string& error)
{
    const int channels = info.channels;
    const size_t blockBytes = static_cast<size_t> (info.blockWords) * 2;

    if (bytes.size() < blockBytes * static_cast<size_t> (stroke.blocks)
        || stroke.frames > maxFrames (stroke.blocks, info.blockWords, channels))
    {
        error = "Piano stroke " + std::to_string (stroke.index) + " doesn't fit its blocks.";
        return false;
    }

    out.assign (static_cast<size_t> (stroke.frames) * static_cast<size_t> (channels), 0);
    auto history = stroke.history;
    int64_t t = 0;

    for (int b = 0; b < stroke.blocks; ++b)
    {
        const uint8_t* block = bytes.data() + static_cast<size_t> (b) * blockBytes;
        const int header = be16 (block);
        const int width = header & 0x1f;
        const int order = (header >> 5) & 7;
        if (width == 0 || order > 4)
        {
            error = "Piano stroke " + std::to_string (stroke.index) + " has a bad block header.";
            return false;
        }

        const int* coef = predictors[order];
        const int own = framesPerBlock (info.blockWords, width, channels);
        const int64_t half = int64_t { 1 } << (width - 1);
        const int64_t full = int64_t { 1 } << width;
        const size_t availableBits = (static_cast<size_t> (info.blockWords) - 1) * 16;
        size_t bit = 0;

        for (int n = 0; n < own; ++n)
        {
            if (t >= stroke.frames)
            {
                error = "Piano stroke " + std::to_string (stroke.index) + " has more frames than its directory says.";
                return false;
            }
            for (int c = 0; c < channels; ++c)
            {
                if (bit + static_cast<size_t> (width) > availableBits)
                {
                    error = "Piano stroke " + std::to_string (stroke.index) + " overruns a block.";
                    return false;
                }
                // `width` bits, LSB-first across consecutive 16-bit words.
                uint64_t v = 0;
                for (int got = 0; got < width;)
                {
                    const unsigned word = be16 (block + 2 + (bit >> 4) * 2);
                    const int offs = static_cast<int> (bit & 15);
                    const int take = std::min (16 - offs, width - got);
                    v |= static_cast<uint64_t> ((word >> offs) & ((1u << take) - 1)) << got;
                    got += take;
                    bit += static_cast<size_t> (take);
                }
                const int64_t residual = static_cast<int64_t> (v) >= half ? static_cast<int64_t> (v) - full : static_cast<int64_t> (v);

                auto& h = history[static_cast<size_t> (c)];
                int64_t x = residual;
                for (int k = 0; k < order; ++k)
                    x += coef[k] * h[static_cast<size_t> (k)];
                h[3] = h[2]; h[2] = h[1]; h[1] = h[0]; h[0] = x;

                out[static_cast<size_t> (t * channels + c)] = static_cast<int32_t> (std::clamp<int64_t> (x, INT32_MIN, INT32_MAX));
            }
            ++t;
        }
    }

    if (t != stroke.frames)
    {
        error = "Piano stroke " + std::to_string (stroke.index) + " decoded to the wrong length.";
        return false;
    }
    return true;
}

double damperT60 (int damperIndex) noexcept
{
    return 0.0015 * std::pow (10.0, damperIndex / 20.0);
}

double touchAttenuation (int velocity) noexcept
{
    const double v = std::clamp (velocity, 1, 127);
    return std::clamp (-200.0 * std::log10 (v / 127.0), 0.0, 200.0);
}

int strokeForVelocity (const PianoInfo& info, int zone, int velocity) noexcept
{
    // Layers switch halfway between neighbouring floors; the velocity's attenuation
    // plays the last layer whose switch point it reaches. No crossfade between layers.
    const double att = touchAttenuation (velocity);
    const Stroke* previous = nullptr;
    int pick = -1;
    for (const auto& s : info.strokes)
    {
        if (s.zone != zone || s.flags != 0)
            continue;
        if (previous == nullptr)
            pick = s.index;
        else
        {
            const int floor = s.attenuationFloor;
            if (floor - (floor - previous->attenuationFloor) / 2 <= att)
                pick = s.index;
        }
        previous = &s;
    }
    return pick;
}

namespace {

/** Turns decoded integer PCM into an engine sample, normalised to full scale. */
void toSample (const std::vector<int32_t>& pcm, const PianoInfo& info, const Stroke& stroke, Sample& sample)
{
    int64_t peak = 0;
    for (auto v : pcm)
        peak = std::max<int64_t> (peak, v < 0 ? -int64_t { v } : int64_t { v });

    int shift = 0;
    while ((peak >> shift) > 32767)
        ++shift;

    sample.channels = info.channels;
    sample.frames = stroke.frames;
    sample.sampleRate = pianoSampleRate;
    sample.data.resize (pcm.size());
    for (size_t i = 0; i < pcm.size(); ++i)
        sample.data[i] = static_cast<int16_t> (pcm[i] >> shift);

    // OpenNord brings each recording to full scale and lets the touch set the level:
    // a zone's layers peak alike at the same attenuation.
    sample.gain = peak > 0 ? static_cast<float> (std::ldexp (1.0, shift) / static_cast<double> (peak)) : 0.0f;

    if (stroke.loopLength > 0 && stroke.loopStart + stroke.loopLength <= stroke.frames)
    {
        sample.loopStart = stroke.loopStart;
        sample.loopLength = stroke.loopLength;
        if (stroke.loopDecay > 0.0 && stroke.loopDecay < 1.0)
            sample.loopDecayPerSecond = envelopeRate * std::log (stroke.loopDecay);
    }

    sample.tuneCents = static_cast<float> (stroke.fineTune);

    // The key-up damper, when the piano uses one (release mode 0xFF means none). A
    // T60 below 20 ms or above 5 s is taken as unreadable and the default fade is kept.
    if (info.releaseMode != 0xff && stroke.damperIndex > 0)
    {
        const double t60 = damperT60 (stroke.damperIndex);
        if (t60 >= 0.02 && t60 <= 5.0)
            sample.releaseTimeConstant = static_cast<float> (t60 / std::log (1000.0));
    }
}

std::shared_ptr<Instrument> buildInstrument (const PianoInfo& info,
                                             const std::function<ByteReader()>& makeReader,
                                             std::string& error, LoadOptions options)
{
    if (info.zones.empty() || static_cast<int> (info.zones.size()) != info.strokeZoneCount())
    {
        error = "This piano's key map doesn't match its recordings, so it can't be played.";
        return nullptr;
    }

    auto instrument = std::make_shared<Instrument>();
    instrument->name = info.name.empty() ? "Nord piano" : info.name;
    instrument->zoneForKey.fill (-1);
    instrument->undampedFromKey = std::clamp (info.undampedFrom, 0, 128);

    // A hard-struck note peaks near -15 dBFS on the instrument itself: each stroke's
    // level word (+0x34) cancels its recorded level, about 0.1 dB a step, so every layer
    // plays at the same level before touch, and the trims and a fixed 6 dB pull it down.
    // Matching that leaves room for big pedalled chords before the limiter has to act.
    instrument->outputGain = 0.18f;

    for (size_t k = 0; k < 128; ++k)
        instrument->keyTuneCents[k] = static_cast<float> (info.keyTuneCents[k]);

    // Pick the stroke for every zone and velocity; decode only the ones that play.
    std::map<int, int32_t> sampleOfStroke;
    for (size_t z = 0; z < info.zones.size(); ++z)
    {
        const auto& kz = info.zones[z];
        Zone zone;
        zone.rootKey = kz.rootNote + rootOffsetSemitones;
        zone.sampleForVelocity.fill (-1);
        for (int v = 0; v < 128; ++v)
        {
            const int stroke = strokeForVelocity (info, static_cast<int> (z), std::max (v, 1));
            if (stroke < 0)
                continue;
            auto [it, inserted] = sampleOfStroke.try_emplace (stroke, static_cast<int32_t> (sampleOfStroke.size()));
            zone.sampleForVelocity[static_cast<size_t> (v)] = it->second;
        }
        for (int key = std::max (kz.lowNote, 0); key <= std::min (kz.highNote, 127); ++key)
            instrument->zoneForKey[static_cast<size_t> (key)] = static_cast<int16_t> (z);
        instrument->zones.push_back (zone);
    }

    // Per-key voicing: the header's level trims, 0.1 dB a step, relative to the loudest key.
    int loudestTrim = INT32_MIN;
    for (size_t k = 0; k < 128; ++k)
        if (instrument->zoneForKey[k] >= 0)
            loudestTrim = std::max (loudestTrim, info.keyLevelTrim[k]);
    for (size_t k = 0; k < 128; ++k)
        if (instrument->zoneForKey[k] >= 0)
            instrument->keyGain[k] = static_cast<float> (std::pow (10.0, std::max (info.keyLevelTrim[k] - loudestTrim, -200) * 0.1 / 20.0));

    for (int v = 0; v < 128; ++v)
        instrument->velocityGain[static_cast<size_t> (v)] = static_cast<float> (std::pow (10.0, -touchAttenuation (std::max (v, 1)) / 100.0));

    std::vector<int> strokes (sampleOfStroke.size());
    for (const auto& [stroke, sample] : sampleOfStroke)
        strokes[static_cast<size_t> (sample)] = stroke;
    instrument->samples.resize (strokes.size());

    // Decode in parallel: each worker reads and decodes whole strokes.
    std::atomic<size_t> next { 0 }, done { 0 };
    std::atomic<bool> failed { false };
    std::mutex errorLock;
    const size_t blockBytes = static_cast<size_t> (info.blockWords) * 2;

    auto work = [&]
    {
        const auto read = makeReader();
        std::vector<uint8_t> blocks;
        std::vector<int32_t> pcm;
        std::string message;

        for (size_t i; (i = next++) < strokes.size();)
        {
            if (failed || (options.cancel != nullptr && options.cancel->load()))
                return;

            const auto& stroke = info.strokes[static_cast<size_t> (strokes[i])];
            blocks.resize (blockBytes * static_cast<size_t> (stroke.blocks));
            bool ok = read (stroke.offset, blocks.size(), blocks.data());
            if (! ok)
                message = "Couldn't read the piano file.";
            else
                ok = decodeStroke (blocks, info, stroke, pcm, message);

            if (! ok)
            {
                std::lock_guard lock (errorLock);
                if (! failed.exchange (true))
                    error = message;
                return;
            }

            toSample (pcm, info, stroke, instrument->samples[i]);
            const auto finished = ++done;
            if (options.progress != nullptr)
                options.progress->store (static_cast<float> (finished) / static_cast<float> (strokes.size()));
        }
    };

    unsigned threads = options.threads > 0 ? static_cast<unsigned> (options.threads) : std::max (1u, std::thread::hardware_concurrency());
    threads = std::min<unsigned> (threads, static_cast<unsigned> (std::max<size_t> (strokes.size(), 1)));
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; ++t)
        pool.emplace_back (work);
    work();
    for (auto& t : pool)
        t.join();

    if (failed)
        return nullptr;
    if (options.cancel != nullptr && options.cancel->load())
    {
        error = "Loading was cancelled.";
        return nullptr;
    }
    return instrument;
}

} // namespace

std::shared_ptr<Instrument> loadPiano (std::span<const uint8_t> file, std::string& error, LoadOptions options)
{
    auto makeReader = [file]() -> ByteReader
    {
        return [file] (uint64_t offset, size_t count, uint8_t* destination)
        {
            if (offset > file.size() || count > file.size() - offset)
                return false;
            std::memcpy (destination, file.data() + offset, count);
            return true;
        };
    };

    PianoInfo info;
    if (! readPianoInfo (makeReader(), file.size(), info, error))
        return nullptr;
    return buildInstrument (info, makeReader, error, options);
}

std::shared_ptr<Instrument> loadPiano (const std::filesystem::path& path, std::string& error, LoadOptions options)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size (path, ec);
    if (ec)
    {
        error = "Couldn't open " + path.filename().string() + ".";
        return nullptr;
    }

    // Each worker opens its own stream so reads never contend for a file position.
    auto makeReader = [path]() -> ByteReader
    {
        auto stream = std::make_shared<std::ifstream> (path, std::ios::binary);
        return [stream] (uint64_t offset, size_t count, uint8_t* destination)
        {
            stream->clear();
            stream->seekg (static_cast<std::streamoff> (offset));
            stream->read (reinterpret_cast<char*> (destination), static_cast<std::streamsize> (count));
            return stream->gcount() == static_cast<std::streamsize> (count);
        };
    };

    PianoInfo info;
    if (! readPianoInfo (makeReader(), size, info, error))
        return nullptr;
    return buildInstrument (info, makeReader, error, options);
}

} // namespace ck::nord
