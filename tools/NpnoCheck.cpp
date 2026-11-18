// Loads Nord pianos the way the app does and prints what it found. Nothing is written.
// Usage: customkeys_npno_check <file-or-folder>...
//        customkeys_npno_check --stress <file>   plays a dense pedalled passage offline
//                                                 and prints load, voices, steals, limiting

#include "customkeys/Engine.h"
#include "customkeys/NordPiano.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace {

/** Renders 20 s of heavy pedalled playing at 48 kHz in 128-sample blocks, timing each block. */
int stress (const fs::path& file)
{
    std::string error;
    auto instrument = ck::nord::loadPiano (file, error);
    if (instrument == nullptr)
    {
        std::printf ("FAIL  %s: %s\n", file.filename().string().c_str(), error.c_str());
        return 1;
    }

    constexpr double rate = 48000.0;
    constexpr int block = 128;
    ck::Engine engine;
    engine.prepare (rate);
    engine.setInstrument (instrument);

    std::vector<float> left (block), right (block);
    float* out[] = { left.data(), right.data() };
    std::vector<ck::MidiEvent> events;
    uint32_t seed = 12345;
    auto random = [&seed] (int n) { seed = seed * 1664525u + 1013904223u; return static_cast<int> ((seed >> 8) % static_cast<uint32_t> (n)); };

    const int blocks = static_cast<int> (20.0 * rate / block);
    const int chordEvery = static_cast<int> (0.15 * rate / block);   // a 10-note chord every 150 ms
    const int pedalEvery = static_cast<int> (2.0 * rate / block);    // re-pedal every 2 s
    double worst = 0.0, total = 0.0;
    float outputPeak = 0.0f;
    int overBudget = 0;
    std::vector<int> held;

    for (int b = 0; b < blocks; ++b)
    {
        events.clear();
        if (b % pedalEvery == 0)
        {
            events.push_back (ck::MidiEvent::sustain (0, false));
            events.push_back (ck::MidiEvent::sustain (1, true));
        }
        if (b % chordEvery == 0)
        {
            for (int note : held)
                events.push_back (ck::MidiEvent::noteOff (2, static_cast<uint8_t> (note)));
            held.clear();
            const int base = 28 + random (60);
            for (int k = 0; k < 10; ++k)
            {
                const int note = std::min (base + k * 3 + random (3), 106);
                held.push_back (note);
                events.push_back (ck::MidiEvent::noteOn (3, static_cast<uint8_t> (note), static_cast<uint8_t> (60 + random (68))));
            }
        }

        const auto start = std::chrono::steady_clock::now();
        engine.process (out, 2, block, events);
        const double load = std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count() / (block / rate);
        worst = std::max (worst, load);
        total += load;
        overBudget += load >= 1.0 ? 1 : 0;
        for (int i = 0; i < block; ++i)
            outputPeak = std::max ({ outputPeak, std::abs (left[static_cast<size_t> (i)]), std::abs (right[static_cast<size_t> (i)]) });
    }

    const auto stats = engine.stats().snapshot();
    std::printf ("stress %s: 20 s at 48 kHz / 128 samples, polyphony %d\n"
                 "  DSP load: average %.0f%%, worst block %.0f%%, blocks over budget %d\n"
                 "  voices: peak %d, stolen %u\n"
                 "  output: peak %.2f, blocks the limiter acted on %u of %d\n",
                 file.filename().string().c_str(), engine.maxPolyphony(),
                 100.0 * total / blocks, 100.0 * worst, overBudget,
                 stats.peakVoices, stats.steals,
                 static_cast<double> (outputPeak), stats.limitedBlocks, blocks);
    return 0;
}

} // namespace

int main (int argc, char** argv)
{
    if (argc == 3 && std::strcmp (argv[1], "--stress") == 0)
        return stress (argv[2]);

    std::vector<fs::path> files;
    for (int i = 1; i < argc; ++i)
    {
        if (argv[i][0] == '-')
        {
            std::printf ("Usage: customkeys_npno_check <file-or-folder>... | --stress <file>\n");
            return 2;
        }
        const fs::path p (argv[i]);
        if (fs::is_directory (p))
        {
            for (const auto& e : fs::recursive_directory_iterator (p, fs::directory_options::skip_permission_denied))
                if (e.is_regular_file() && e.path().extension() == ".npno")
                    files.push_back (e.path());
        }
        else
            files.push_back (p);
    }

    int failures = 0;
    for (const auto& file : files)
    {
        const auto start = std::chrono::steady_clock::now();
        std::string error;
        auto instrument = ck::nord::loadPiano (file, error);
        const std::chrono::duration<double> seconds = std::chrono::steady_clock::now() - start;

        if (instrument == nullptr)
        {
            ++failures;
            std::printf ("FAIL  %s: %s\n", file.filename().string().c_str(), error.c_str());
            continue;
        }

        int lowest = 128, highest = -1;
        for (int k = 0; k < 128; ++k)
            if (instrument->zoneForKey[static_cast<size_t> (k)] >= 0) { lowest = std::min (lowest, k); highest = k; }

        std::printf ("ok    %s: \"%s\", %zu zones (keys %d-%d), %zu recordings, %.0f MB, %.1f s\n",
                     file.filename().string().c_str(), instrument->name.c_str(), instrument->zones.size(),
                     lowest, highest, instrument->samples.size(),
                     static_cast<double> (instrument->sampleBytes()) / (1024.0 * 1024.0), seconds.count());
    }

    std::printf ("\n%zu pianos, %d failed\n", files.size(), failures);
    return failures == 0 ? 0 : 1;
}
