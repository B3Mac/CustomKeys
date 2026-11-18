#pragma once

#include "customkeys/Instrument.h"

#include <filesystem>
#include <string>

namespace ck::audio {

/** Extra facts some audio files carry about how they are meant to be played. */
struct FileInfo
{
    int unityNote = -1;     ///< MIDI note the recording was made at (WAV `smpl` chunk), or -1.
    double unityCents = 0.0; ///< Fine tuning that goes with unityNote.
    int bitsPerSample = 0;   ///< As stored in the file.
    int fileChannels = 0;    ///< Before any extra channels are dropped.
};

/** True for the extensions readAudioFile understands: .wav, .wave, .aif, .aiff, .aifc and .flac. */
bool isAudioFile (const std::filesystem::path& path);

/** Reads a WAV (PCM or float, including WAVE_FORMAT_EXTENSIBLE), AIFF/AIFC or FLAC
    file into a Sample: 16-bit, interleaved, at most two channels (extra ones are
    dropped). A sustain loop in a WAV `smpl` chunk becomes the sample's loop.
    Not real-time safe: call it from a loader thread. */
bool readAudioFile (const std::filesystem::path& path, Sample& sample, FileInfo& info, std::string& error);

} // namespace ck::audio
