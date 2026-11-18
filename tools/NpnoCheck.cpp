// Loads Nord pianos the way the app does and prints what it found. Nothing is written.
// Usage: customkeys_npno_check <file-or-folder>...

#include "customkeys/NordPiano.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

int main (int argc, char** argv)
{
    std::vector<fs::path> files;
    for (int i = 1; i < argc; ++i)
    {
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
