#pragma once

#include "customkeys/Instrument.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/** Loaders for open sample formats. Each builds the same ck::Instrument the
    engine plays. None of them are real-time safe: run them on a loader thread. */
namespace ck::library {

struct LoadOptions
{
    std::atomic<float>* progress = nullptr;     ///< 0..1 while loading, if set.
    const std::atomic<bool>* cancel = nullptr;  ///< Stop early when this becomes true.
};

/** What a load did, for the status line and for debugging a library. */
struct LoadReport
{
    int filesLoaded = 0;
    int keysMapped = 0;
    std::vector<std::string> warnings; ///< Things skipped or approximated, one line each.
};

/** Loads anything CustomKeys reads as open formats: an .sfz file, a single
    audio file (WAV, AIFF, FLAC), or a folder of audio files. */
std::shared_ptr<Instrument> load (const std::filesystem::path& path, std::string& error,
                                  LoadReport* report = nullptr, const LoadOptions& options = {});

/** True when load() can open this path. */
bool canLoad (const std::filesystem::path& path);

// ---------------------------------------------------------------------------
// SFZ

/** One <region> after <global>, <master> and <group> opcodes are merged in. */
struct SfzRegion
{
    std::filesystem::path sample; ///< Resolved against the .sfz folder and default_path.
    int loKey = 0, hiKey = 127; ///< Both -1 when a controller, not a key, plays the region.
    int loVel = 1, hiVel = 127;
    double keyCenter = 60.0;
    bool keyCenterFromSample = false; ///< pitch_keycenter=sample
    double tuneCents = 0.0;
    int transpose = 0;
    double volumeDb = 0.0;
    double ampVeltrack = 100.0;
    double ampegRelease = 0.001;
    std::string trigger = "attack";
    std::string loopMode;           ///< Empty when not set.
    int64_t loopStart = -1, loopEnd = -1;
    int seqPosition = 1;
    double loRand = 0.0, hiRand = 1.0;
    int loCC64 = 0, hiCC64 = 127;
    int swLast = -1;    ///< Key switch that selects this region, or -1.
    int swDefault = -1; ///< Key switch active before any is pressed, or -1.
};

/** Parses SFZ text. `sfzFolder` resolves relative sample and #include paths.
    Unknown opcodes are ignored. */
std::vector<SfzRegion> parseSfz (std::string_view text, const std::filesystem::path& sfzFolder,
                                 std::vector<std::string>* warnings = nullptr);

/** Note name or number to MIDI key: "60", "c4", "C#4", "db4" (C4 = 60). -1 if not a key. */
int parseSfzKey (std::string_view value);

// ---------------------------------------------------------------------------
// Folders of samples

/** What a sample's file name says about it. */
struct SampleName
{
    int note = -1;       ///< MIDI note, C4 = 60.
    int velocity = -1;   ///< A number from the name (v80, vel100, a trailing 100...), or -1.
    int dynamic = -1;    ///< ppp=0 ... fff=7 when the name uses dynamic markings, else -1.
    int roundRobin = 0;  ///< rr2 = 2; 0 when not given.
    bool release = false;
};

/** Reads note, velocity and round robin from a file name without its extension,
    e.g. "Grand_C#3_v80_rr1", "piano-060-127", "Upright A0 ff". */
std::optional<SampleName> parseSampleName (std::string_view stem);

} // namespace ck::library
