// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Reader for Nord Piano Library files (.npno), ported to C++ from OpenNord
// (https://github.com/simonflore/opennord, src/lib/ns4/nsp.ts and nsp-audio.ts),
// Copyright the OpenNord contributors, licensed under the GNU AGPL v3.
// See engine/src/nord/NOTICE.md.
//
// Files are read and decoded on the user's own machine at load time. No Nord
// audio is stored in this repository or written anywhere by CustomKeys.

#pragma once

#include "customkeys/Instrument.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ck::nord {

/** Recordings sound this many semitones above their key-map root at 44.1 kHz. */
inline constexpr int rootOffsetSemitones = 4;
inline constexpr double pianoSampleRate = 44100.0;
/** Envelope updates a second on the instrument; a stroke's loop decay applies once per update. */
inline constexpr double envelopeRate = 22050.0;

/** The key range one sampled note covers. */
struct KeyZone
{
    int rootNote = 0; ///< Key-map root; the recording sounds at rootNote + rootOffsetSemitones.
    int lowNote = 0;
    int highNote = 0;
};

/** One recording: a velocity layer (or release sound) of one key zone. */
struct Stroke
{
    int index = 0;
    int zone = 0;
    int attenuationFloor = 0; ///< 0.2 dB steps, 0 = the hardest hit. Layers run loudest first.
    int flags = 0;            ///< 0 normal, 1 alternative list, 2 key-release sound.
    int64_t frames = 0;
    uint64_t offset = 0;      ///< File offset of the first block.
    int blocks = 0;
    std::array<std::array<int64_t, 4>, 2> history {}; ///< Predictor seed per channel, newest first.
    int64_t loopStart = 0;
    int64_t loopLength = 0;   ///< 0 = no loop.
    double loopDecay = 1.0;   ///< Level factor per envelope update inside the loop.
};

/** Everything in a .npno except the audio itself. */
struct PianoInfo
{
    std::string name;    ///< e.g. "Grand Imperial".
    std::string variant; ///< After the '#' in the name field, may be empty.
    std::string version; ///< e.g. "6.10".
    int channels = 1;
    int blockWords = 511;
    std::vector<KeyZone> zones; ///< Low to high.
    std::vector<Stroke> strokes;

    /** How many key zones the strokes make up. */
    int strokeZoneCount() const noexcept;
};

/** Reads `count` bytes at `offset` into `destination`; false on a short read. */
using ByteReader = std::function<bool (uint64_t offset, size_t count, uint8_t* destination)>;

/** Parses the header, key map and stroke directory. False (with a message) when
    the file isn't a readable Nord Piano. */
bool readPianoInfo (const ByteReader& read, uint64_t fileSize, PianoInfo& info, std::string& error);

/** Decodes one stroke to interleaved integer PCM. `blocks` holds exactly the
    stroke's blocks (stroke.blocks * info.blockWords * 2 bytes). */
bool decodeStroke (std::span<const uint8_t> blocks, const PianoInfo& info, const Stroke& stroke,
                   std::vector<int32_t>& interleaved, std::string& error);

/** Velocity (1-127) to attenuation in 0.2 dB steps: a 40 log10(v/127) dB law, as OpenNord uses. */
double touchAttenuation (int velocity) noexcept;

/** The stroke a zone plays at `velocity`, the way the instrument picks it, or -1. */
int strokeForVelocity (const PianoInfo& info, int zone, int velocity) noexcept;

struct LoadOptions
{
    std::atomic<float>* progress = nullptr; ///< 0..1 while decoding, if set.
    const std::atomic<bool>* cancel = nullptr;
    int threads = 0; ///< 0 = one per core.
};

/** Builds a playable instrument from a .npno file: every velocity layer of every
    key zone is decoded into memory. Returns null with a message on failure. */
std::shared_ptr<Instrument> loadPiano (const std::filesystem::path& file, std::string& error, LoadOptions options = {});

/** The same, from a file already in memory (tests). */
std::shared_ptr<Instrument> loadPiano (std::span<const uint8_t> file, std::string& error, LoadOptions options = {});

/** True if `firstBytes` (at least 12) look like a Nord Piano file. */
bool looksLikePiano (std::span<const uint8_t> firstBytes) noexcept;

} // namespace ck::nord
