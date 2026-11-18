// Loads Nord pianos the way the app does and prints what it found. Nothing is written.
// Usage: customkeys_npno_check <file-or-folder>...
//        customkeys_npno_check --stress <file> [rate]  plays a dense pedalled passage offline
//                                                 and prints load, voices, steals, limiting
//        customkeys_npno_check --analyze <file>  per zone: measured pitch against the
//                                                 expected one, loop points and levels.
// Only numbers are printed; no audio is written anywhere.

#include "customkeys/Engine.h"
#include "customkeys/NordPiano.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

namespace {

/** Renders 20 s of heavy pedalled playing at 48 kHz in 128-sample blocks, timing each block. */
int stress (const fs::path& file, double rate)
{
    std::string error;
    auto instrument = ck::nord::loadPiano (file, error);
    if (instrument == nullptr)
    {
        std::printf ("FAIL  %s: %s\n", file.filename().string().c_str(), error.c_str());
        return 1;
    }

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
    std::printf ("stress %s: 20 s at %.0f Hz / 128 samples, polyphony %d\n"
                 "  DSP load: average %.0f%%, worst block %.0f%%, blocks over budget %d\n"
                 "  voices: peak %d, stolen %u\n"
                 "  output: peak %.2f, blocks the limiter acted on %u of %d\n",
                 file.filename().string().c_str(), rate, engine.maxPolyphony(),
                 100.0 * total / blocks, 100.0 * worst, overBudget,
                 stats.peakVoices, stats.steals,
                 static_cast<double> (outputPeak), stats.limitedBlocks, blocks);
    return 0;
}

/** Period (in samples) near `expected` with the lowest YIN difference, or 0. */
double yinPeriod (const std::vector<double>& x, size_t start, double expected, double& quality)
{
    const int lo = std::max (2, static_cast<int> (expected / 1.15)), hi = static_cast<int> (expected * 1.15) + 2;
    const size_t window = static_cast<size_t> (std::max (2048.0, 2.5 * expected));
    if (start + window + static_cast<size_t> (hi) + 2 > x.size())
        return 0.0;

    std::vector<double> d (static_cast<size_t> (hi + 2), 0.0), cmnd (d.size(), 1.0);
    double running = 0.0;
    for (int tau = 1; tau <= hi + 1; ++tau)
    {
        double sum = 0.0;
        for (size_t i = 0; i < window; ++i)
        {
            const double diff = x[start + i] - x[start + i + static_cast<size_t> (tau)];
            sum += diff * diff;
        }
        d[static_cast<size_t> (tau)] = sum;
        running += sum;
        cmnd[static_cast<size_t> (tau)] = running > 0.0 ? sum * tau / running : 1.0;
    }
    int best = lo;
    for (int tau = lo; tau <= hi; ++tau)
        if (cmnd[static_cast<size_t> (tau)] < cmnd[static_cast<size_t> (best)])
            best = tau;
    quality = cmnd[static_cast<size_t> (best)];
    if (best <= lo || best >= hi)
        return 0.0; // minimum at the edge of the search: not near the expected pitch
    const double a = cmnd[static_cast<size_t> (best - 1)], b = cmnd[static_cast<size_t> (best)], c = cmnd[static_cast<size_t> (best + 1)];
    const double denom = a - 2.0 * b + c;
    return best + (denom != 0.0 ? 0.5 * (a - c) / denom : 0.0);
}

/** Normalised correlation of the `n` samples before `a` and before `b` (1 = identical). */
double seamCorrelation (const std::vector<double>& x, int64_t a, int64_t b, int64_t n = 64)
{
    if (a < n || b < n || a > static_cast<int64_t> (x.size()) || b > static_cast<int64_t> (x.size()))
        return std::nan ("");
    double xy = 0, xx = 0, yy = 0;
    for (int64_t i = 1; i <= n; ++i)
    {
        const double p = x[static_cast<size_t> (a - i)], q = x[static_cast<size_t> (b - i)];
        xy += p * q; xx += p * p; yy += q * q;
    }
    return xx > 0 && yy > 0 ? xy / std::sqrt (xx * yy) : std::nan ("");
}

int analyze (const fs::path& file)
{
    std::ifstream stream (file, std::ios::binary);
    std::error_code ec;
    const auto size = fs::file_size (file, ec);
    ck::nord::ByteReader read = [&stream] (uint64_t offset, size_t count, uint8_t* destination)
    {
        stream.clear();
        stream.seekg (static_cast<std::streamoff> (offset));
        stream.read (reinterpret_cast<char*> (destination), static_cast<std::streamsize> (count));
        return stream.gcount() == static_cast<std::streamsize> (count);
    };

    ck::nord::PianoInfo info;
    std::string error;
    if (ec || ! ck::nord::readPianoInfo (read, size, info, error))
    {
        std::printf ("FAIL  %s: %s\n", file.filename().string().c_str(), error.c_str());
        return 1;
    }

    std::map<int, int> flagCounts;
    for (const auto& s : info.strokes)
        ++flagCounts[s.flags];
    std::printf ("analyze %s: \"%s\" v%s, %d channels, %zu key zones, %zu strokes (flags:",
                 file.filename().string().c_str(), info.name.c_str(), info.version.c_str(), info.channels,
                 info.zones.size(), info.strokes.size());
    for (auto [f, n] : flagCounts)
        std::printf (" %d=%d", f, n);
    std::printf (")\n");
    std::printf ("zone root keys  layers  loudest: secs  loop(start+len/frames)  seam  seam/2   cents  yin    peak  avgdB\n");

    std::vector<double> cents;
    for (size_t z = 0; z < info.zones.size(); ++z)
    {
        const auto& kz = info.zones[z];
        int layers = 0;
        const ck::nord::Stroke* loud = nullptr;
        for (const auto& s : info.strokes)
            if (s.zone == static_cast<int> (z) && s.flags == 0) { ++layers; if (loud == nullptr) loud = &s; }
        if (loud == nullptr) { std::printf ("%4zu %4d %3d-%-3d %3d  (no normal strokes)\n", z, kz.rootNote, kz.lowNote, kz.highNote, layers); continue; }

        std::vector<uint8_t> blocks (static_cast<size_t> (loud->blocks) * static_cast<size_t> (info.blockWords) * 2);
        std::vector<int32_t> pcm;
        if (! read (loud->offset, blocks.size(), blocks.data()) || ! ck::nord::decodeStroke (blocks, info, *loud, pcm, error))
        {
            std::printf ("%4zu decode failed: %s\n", z, error.c_str());
            continue;
        }
        std::vector<double> mono (static_cast<size_t> (loud->frames));
        double peak = 0, energy = 0;
        for (size_t n = 0; n < mono.size(); ++n)
        {
            double v = 0;
            for (int c = 0; c < info.channels; ++c) v += pcm[n * static_cast<size_t> (info.channels) + static_cast<size_t> (c)];
            mono[n] = v / info.channels;
            peak = std::max (peak, std::abs (mono[n]));
            energy += mono[n] * mono[n];
        }
        const double rms = std::sqrt (energy / std::max<size_t> (mono.size(), 1));

        const double expectedHz = 440.0 * std::pow (2.0, (kz.rootNote + ck::nord::rootOffsetSemitones - 69) / 12.0);
        const double expectedPeriod = ck::nord::pianoSampleRate / expectedHz;
        const size_t at = std::min<size_t> (static_cast<size_t> (0.3 * ck::nord::pianoSampleRate), mono.size() / 4);
        double quality = 1.0;
        const double period = yinPeriod (mono, at, expectedPeriod, quality);
        const double dev = period > 0 ? 1200.0 * std::log2 (expectedPeriod / period) : std::nan ("");
        if (period > 0) cents.push_back (dev);

        const int64_t end = loud->loopStart + loud->loopLength;
        const double seam = loud->loopLength > 0 ? seamCorrelation (mono, end, loud->loopStart) : std::nan ("");
        const double seamHalf = loud->loopLength > 0 ? seamCorrelation (mono, end / 2, loud->loopStart / 2) : std::nan ("");

        std::printf ("%4zu %4d %3d-%-3d %4d  %13.2f  %8lld+%-7lld/%-8lld %5.2f  %5.2f  %+7.1f  %.2f  %5.0f  %5.1f\n",
                     z, kz.rootNote, kz.lowNote, kz.highNote, layers,
                     static_cast<double> (loud->frames) / ck::nord::pianoSampleRate,
                     static_cast<long long> (loud->loopStart), static_cast<long long> (loud->loopLength), static_cast<long long> (loud->frames),
                     seam, seamHalf, dev, quality, peak, 20.0 * std::log10 (std::max (rms, 1.0) / std::max (peak, 1.0)));
    }

    if (! cents.empty())
    {
        std::sort (cents.begin(), cents.end());
        std::printf ("pitch vs expected (root + %d semitones at 44.1 kHz): median %+.1f cents, min %+.1f, max %+.1f over %zu zones\n",
                     ck::nord::rootOffsetSemitones, cents[cents.size() / 2], cents.front(), cents.back(), cents.size());
    }
    return 0;
}

} // namespace

int main (int argc, char** argv)
{
    if ((argc == 3 || argc == 4) && std::strcmp (argv[1], "--stress") == 0)
        return stress (argv[2], argc == 4 ? std::atof (argv[3]) : 48000.0);
    if (argc == 3 && std::strcmp (argv[1], "--analyze") == 0)
        return analyze (argv[2]);

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
