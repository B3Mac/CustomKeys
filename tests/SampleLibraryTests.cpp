#include "TestFramework.h"

#include "customkeys/AudioFile.h"
#include "customkeys/Engine.h"
#include "customkeys/SampleLibrary.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace ck::library;

namespace {

// ---------------------------------------------------------------------------
// Synthetic files. No real sample libraries are used or stored by these tests.

struct TempDir
{
    fs::path path;
    TempDir()
    {
        static std::atomic<int> counter { 0 };
        path = fs::temp_directory_path() / ("customkeys-test-" + std::to_string (counter++) + "-"
                                            + std::to_string (std::hash<std::string> {} (fs::current_path().string()) % 100000));
        fs::remove_all (path);
        fs::create_directories (path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all (path, ec); }
};

void put16le (std::vector<uint8_t>& b, uint32_t v) { b.push_back (uint8_t (v)); b.push_back (uint8_t (v >> 8)); }
void put32le (std::vector<uint8_t>& b, uint32_t v) { put16le (b, v & 0xFFFF); put16le (b, v >> 16); }
void put16be (std::vector<uint8_t>& b, uint32_t v) { b.push_back (uint8_t (v >> 8)); b.push_back (uint8_t (v)); }
void put32be (std::vector<uint8_t>& b, uint32_t v) { put16be (b, v >> 16); put16be (b, v & 0xFFFF); }
void tag (std::vector<uint8_t>& b, const char* t) { b.insert (b.end(), t, t + 4); }

std::vector<double> sine (double frequency, double rate, int frames, double amplitude = 0.5)
{
    std::vector<double> x (static_cast<size_t> (frames));
    for (int n = 0; n < frames; ++n)
        x[static_cast<size_t> (n)] = amplitude * std::sin (2.0 * std::numbers::pi * frequency * n / rate);
    return x;
}

void writeFile (const fs::path& path, const std::vector<uint8_t>& bytes)
{
    fs::create_directories (path.parent_path());
    std::ofstream out (path, std::ios::binary);
    out.write (reinterpret_cast<const char*> (bytes.data()), static_cast<std::streamsize> (bytes.size()));
}

/** Mono or stereo WAV. format 1 = PCM (16 or 24 bit), 3 = float32. Optional smpl loop and unity note. */
void writeWav (const fs::path& path, const std::vector<double>& x, int channels = 1, int bits = 16, int format = 1,
               int64_t loopStart = -1, int64_t loopEnd = -1, int unityNote = -1, double rate = 44100.0)
{
    std::vector<uint8_t> data;
    for (double v : x)
        for (int c = 0; c < channels; ++c)
        {
            const double value = c == 0 ? v : -v;
            if (format == 3)
            {
                float f = float (value);
                uint32_t raw;
                std::memcpy (&raw, &f, 4);
                put32le (data, raw);
            }
            else
            {
                const auto scaled = int32_t (std::lround (value * ((1 << (bits - 1)) - 1)));
                for (int byte = 0; byte < bits / 8; ++byte)
                    data.push_back (uint8_t (uint32_t (scaled) >> (8 * byte)));
            }
        }

    std::vector<uint8_t> b;
    tag (b, "RIFF"); put32le (b, 0); tag (b, "WAVE");
    tag (b, "fmt "); put32le (b, 16);
    put16le (b, uint32_t (format)); put16le (b, uint32_t (channels)); put32le (b, uint32_t (rate));
    put32le (b, uint32_t (rate) * uint32_t (channels * bits / 8)); put16le (b, uint32_t (channels * bits / 8)); put16le (b, uint32_t (bits));
    if (loopStart >= 0 || unityNote >= 0)
    {
        tag (b, "smpl"); put32le (b, 36 + (loopStart >= 0 ? 24 : 0));
        put32le (b, 0); put32le (b, 0); put32le (b, 0);
        put32le (b, uint32_t (unityNote >= 0 ? unityNote : 60)); put32le (b, 0);
        put32le (b, 0); put32le (b, 0);
        put32le (b, loopStart >= 0 ? 1 : 0); put32le (b, 0);
        if (loopStart >= 0)
        {
            put32le (b, 0); put32le (b, 0); put32le (b, uint32_t (loopStart)); put32le (b, uint32_t (loopEnd)); put32le (b, 0); put32le (b, 0);
        }
    }
    tag (b, "data"); put32le (b, uint32_t (data.size()));
    b.insert (b.end(), data.begin(), data.end());
    const auto riffSize = uint32_t (b.size() - 8);
    std::memcpy (b.data() + 4, &riffSize, 4); // little-endian hosts only, which is every CI runner
    writeFile (path, b);
}

/** 16-bit big-endian AIFF, or AIFC "sowt" (little-endian) when sowt is true. */
void writeAiff (const fs::path& path, const std::vector<double>& x, bool sowt = false, double rate = 44100.0)
{
    std::vector<uint8_t> b;
    tag (b, "FORM"); put32be (b, 0); tag (b, sowt ? "AIFC" : "AIFF");
    tag (b, "COMM"); put32be (b, sowt ? 24 : 18);
    put16be (b, 1); put32be (b, uint32_t (x.size())); put16be (b, 16);
    // 80-bit extended sample rate
    int exponent = 0;
    double m = std::frexp (rate, &exponent); // rate = m * 2^exponent, 0.5 <= m < 1
    const uint64_t mantissa = uint64_t (std::ldexp (m, 64));
    put16be (b, uint32_t (16382 + exponent));
    for (int i = 7; i >= 0; --i) b.push_back (uint8_t (mantissa >> (8 * i)));
    if (sowt) { tag (b, "sowt"); b.push_back (0); b.push_back (0); }
    tag (b, "SSND"); put32be (b, uint32_t (8 + x.size() * 2)); put32be (b, 0); put32be (b, 0);
    for (double v : x)
    {
        const auto s = uint16_t (int16_t (std::lround (v * 32767.0)));
        if (sowt) put16le (b, s); else put16be (b, s);
    }
    writeFile (path, b);
}

// A minimal FLAC encoder (verbatim subframes), enough to exercise the FLAC path.
struct BitWriter
{
    std::vector<uint8_t> bytes;
    uint64_t acc = 0;
    int count = 0;
    void put (uint64_t value, int bits)
    {
        for (int i = bits - 1; i >= 0; --i)
        {
            acc = (acc << 1) | ((value >> i) & 1);
            if (++count == 8) { bytes.push_back (uint8_t (acc)); acc = 0; count = 0; }
        }
    }
};

uint8_t crc8 (const uint8_t* p, size_t n)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < n; ++i)
    {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b) crc = uint8_t ((crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1);
    }
    return crc;
}

uint16_t crc16 (const uint8_t* p, size_t n)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < n; ++i)
    {
        crc = uint16_t (crc ^ (p[i] << 8));
        for (int b = 0; b < 8; ++b) crc = uint16_t ((crc & 0x8000) ? (crc << 1) ^ 0x8005 : crc << 1);
    }
    return crc;
}

void writeFlac (const fs::path& path, const std::vector<double>& x, int rate = 44100)
{
    const int block = 1024;
    BitWriter w;
    for (char c : std::string ("fLaC")) w.put (uint8_t (c), 8);
    w.put (1, 1); w.put (0, 7); w.put (34, 24);                   // last metadata block: STREAMINFO
    w.put (block, 16); w.put (block, 16); w.put (0, 24); w.put (0, 24);
    w.put (uint64_t (rate), 20); w.put (0, 3); w.put (15, 5); w.put (x.size(), 36);
    for (int i = 0; i < 16; ++i) w.put (0, 8);                     // MD5 left empty

    for (size_t start = 0, frame = 0; start < x.size(); start += block, ++frame)
    {
        const size_t n = std::min (size_t (block), x.size() - start);
        BitWriter f;
        f.put (0x3FFE, 14); f.put (0, 1); f.put (0, 1);
        f.put (7, 4); f.put (0, 4); f.put (0, 4); f.put (4, 3); f.put (0, 1); // size in header, rate from STREAMINFO, mono, 16-bit
        f.put (frame, 8);                                                       // frame number (< 128)
        f.put (n - 1, 16);
        f.bytes.push_back (crc8 (f.bytes.data(), f.bytes.size()));
        f.put (0, 1); f.put (1, 6); f.put (0, 1);                               // verbatim subframe
        for (size_t i = 0; i < n; ++i)
            f.put (uint16_t (int16_t (std::lround (x[start + i] * 32767.0))), 16);
        const uint16_t crc = crc16 (f.bytes.data(), f.bytes.size());
        f.bytes.push_back (uint8_t (crc >> 8)); f.bytes.push_back (uint8_t (crc));
        w.bytes.insert (w.bytes.end(), f.bytes.begin(), f.bytes.end());
    }
    writeFile (path, w.bytes);
}

void writeText (const fs::path& path, const std::string& text)
{
    fs::create_directories (path.parent_path());
    std::ofstream (path) << text;
}

float maxAbsDifference (const ck::Sample& s, const std::vector<double>& x, int channel = 0)
{
    float worst = 0.0f;
    for (size_t n = 0; n < x.size(); ++n)
        worst = std::max (worst, std::abs (float (s.data[n * size_t (s.channels) + size_t (channel)]) * s.gain - float (x[n])));
    return worst;
}

int zoneFor (const ck::Instrument& inst, int key) { return inst.zoneForKey[size_t (key)]; }
int sampleFor (const ck::Instrument& inst, int key, int velocity)
{
    const int z = zoneFor (inst, key);
    return z < 0 ? -1 : inst.zones[size_t (z)].sampleForVelocity[size_t (velocity)];
}

} // namespace

// ---------------------------------------------------------------------------
// Audio files

CK_TEST ("audio: 16-bit, 24-bit and float WAV read back accurately")
{
    TempDir dir;
    const auto x = sine (440.0, 44100.0, 2000);
    struct Case { const char* name; int bits; int format; float tolerance; };
    for (const auto& c : { Case { "a.wav", 16, 1, 1.0e-4f }, Case { "b.wav", 24, 1, 1.0e-4f }, Case { "c.wav", 32, 3, 1.0e-4f } })
    {
        writeWav (dir.path / c.name, x, 1, c.bits, c.format);
        ck::Sample s;
        ck::audio::FileInfo info;
        std::string error;
        CK_CHECK (ck::audio::readAudioFile (dir.path / c.name, s, info, error));
        CK_CHECK (s.frames == 2000 && s.channels == 1 && s.sampleRate == 44100.0);
        CK_CHECK (info.bitsPerSample == c.bits);
        CK_CHECK (maxAbsDifference (s, x) < c.tolerance);
    }
}

CK_TEST ("audio: stereo WAV keeps both channels")
{
    TempDir dir;
    const auto x = sine (220.0, 48000.0, 1000);
    writeWav (dir.path / "s.wav", x, 2, 16, 1, -1, -1, -1, 48000.0);
    ck::Sample s;
    ck::audio::FileInfo info;
    std::string error;
    CK_CHECK (ck::audio::readAudioFile (dir.path / "s.wav", s, info, error));
    CK_CHECK (s.channels == 2 && s.sampleRate == 48000.0);
    CK_CHECK (s.data[200] == -s.data[201]); // the right channel was written inverted
}

CK_TEST ("audio: WAV smpl chunk gives the loop and unity note")
{
    TempDir dir;
    writeWav (dir.path / "loop.wav", sine (100.0, 44100.0, 5000), 1, 16, 1, 1000, 3999, 57);
    ck::Sample s;
    ck::audio::FileInfo info;
    std::string error;
    CK_CHECK (ck::audio::readAudioFile (dir.path / "loop.wav", s, info, error));
    CK_CHECK (s.loopStart == 1000 && s.loopLength == 3000);
    CK_CHECK (s.loops());
    CK_CHECK (info.unityNote == 57);
}

CK_TEST ("audio: AIFF and AIFC sowt read back accurately")
{
    TempDir dir;
    const auto x = sine (330.0, 44100.0, 1500);
    writeAiff (dir.path / "a.aif", x);
    writeAiff (dir.path / "b.aifc", x, true);
    for (const char* name : { "a.aif", "b.aifc" })
    {
        ck::Sample s;
        ck::audio::FileInfo info;
        std::string error;
        CK_CHECK (ck::audio::readAudioFile (dir.path / name, s, info, error));
        CK_CHECK (s.frames == 1500 && s.sampleRate == 44100.0);
        CK_CHECK (maxAbsDifference (s, x) < 1.0e-4f);
    }
}

CK_TEST ("audio: FLAC decodes")
{
    TempDir dir;
    const auto x = sine (523.25, 44100.0, 3000);
    writeFlac (dir.path / "a.flac", x);
    ck::Sample s;
    ck::audio::FileInfo info;
    std::string error;
    const bool ok = ck::audio::readAudioFile (dir.path / "a.flac", s, info, error);
    if (! ok) std::printf ("    %s\n", error.c_str());
    CK_CHECK (ok);
    CK_CHECK (s.frames == 3000 && s.sampleRate == 44100.0);
    CK_CHECK (maxAbsDifference (s, x) < 1.0e-4f);
}

CK_TEST ("audio: unreadable files fail with a message, not a crash")
{
    TempDir dir;
    writeText (dir.path / "fake.wav", "this is not audio");
    ck::Sample s;
    ck::audio::FileInfo info;
    std::string error;
    CK_CHECK (! ck::audio::readAudioFile (dir.path / "fake.wav", s, info, error));
    CK_CHECK (! error.empty());
    CK_CHECK (! ck::audio::readAudioFile (dir.path / "missing.flac", s, info, error));
}

// ---------------------------------------------------------------------------
// SFZ parsing

CK_TEST ("sfz: keys parse as numbers or note names")
{
    CK_CHECK (parseSfzKey ("60") == 60);
    CK_CHECK (parseSfzKey ("c4") == 60);
    CK_CHECK (parseSfzKey ("C#4") == 61);
    CK_CHECK (parseSfzKey ("db4") == 61);
    CK_CHECK (parseSfzKey ("a0") == 21);
    CK_CHECK (parseSfzKey ("b3") == 59);
    CK_CHECK (parseSfzKey ("bb3") == 58);
    CK_CHECK (parseSfzKey ("c-1") == 0);
    CK_CHECK (parseSfzKey ("x9") == -1);
    CK_CHECK (parseSfzKey ("200") == -1);
}

CK_TEST ("sfz: headers inherit, later levels override")
{
    const auto regions = parseSfz (R"(
        // comment
        <global> amp_veltrack=50 ampeg_release=0.8
        <group> lovel=1 hivel=64 volume=-6
        <region> sample=Soft C4.wav key=60
        <region> sample=soft/D4.wav lokey=61 hikey=63 pitch_keycenter=62 volume=-3
        <group> lovel=65 hivel=127 /* block
                comment */
        <region> sample=Loud C4.wav key=c4 tune=-12 transpose=1
    )", "/lib");

    CK_CHECK (regions.size() == 3);
    if (regions.size() != 3) return;

    CK_CHECK (regions[0].sample == fs::path ("/lib/Soft C4.wav")); // spaces inside a sample path survive
    CK_CHECK (regions[0].loKey == 60 && regions[0].hiKey == 60 && regions[0].keyCenter == 60.0);
    CK_CHECK (regions[0].loVel == 1 && regions[0].hiVel == 64 && regions[0].volumeDb == -6.0);
    CK_CHECK (regions[0].ampVeltrack == 50.0 && regions[0].ampegRelease == 0.8);

    CK_CHECK (regions[1].sample == fs::path ("/lib/soft/D4.wav"));
    CK_CHECK (regions[1].loKey == 61 && regions[1].hiKey == 63 && regions[1].keyCenter == 62.0);
    CK_CHECK (regions[1].volumeDb == -3.0);

    CK_CHECK (regions[2].loVel == 65 && regions[2].volumeDb == 0.0); // the new group dropped volume
    CK_CHECK (regions[2].tuneCents == -12.0 && regions[2].transpose == 1);
}

CK_TEST ("sfz: control default_path, #define and backslashes")
{
    const auto regions = parseSfz (
        "#define $ROOT 48\n"
        "<control> default_path=samples\\grand\\ \n"
        "<region> sample=$ROOT.wav key=$ROOT trigger=release\n"
        "<region> sample=x.wav lokey=$ROOT hikey=50 seq_position=2 locc64=64 hicc64=127 loop_mode=loop_continuous loop_start=10 loop_end=99\n",
        "/lib");
    CK_CHECK (regions.size() == 2);
    if (regions.size() != 2) return;
    CK_CHECK (regions[0].sample == fs::path ("/lib/samples/grand/48.wav"));
    CK_CHECK (regions[0].loKey == 48 && regions[0].trigger == "release");
    CK_CHECK (regions[1].loKey == 48 && regions[1].seqPosition == 2 && regions[1].loCC64 == 64);
    CK_CHECK (regions[1].loopMode == "loop_continuous" && regions[1].loopStart == 10 && regions[1].loopEnd == 99);
}

CK_TEST ("sfz: #include pulls in another file")
{
    TempDir dir;
    writeText (dir.path / "parts" / "regions.sfz", "<region> sample=a.wav key=60\n<region> sample=b.wav key=62\n");
    const auto regions = parseSfz ("<group> volume=-2\n#include \"parts/regions.sfz\"\n", dir.path);
    CK_CHECK (regions.size() == 2);
    if (regions.size() == 2)
        CK_CHECK (regions[1].volumeDb == -2.0);
}

// ---------------------------------------------------------------------------
// SFZ loading

CK_TEST ("sfz: a two-layer piano loads into zones and velocity tables")
{
    TempDir dir;
    for (const auto* n : { "C4", "E4" })
        for (const auto* layer : { "soft", "loud" })
            writeWav (dir.path / "samples" / (std::string (layer) + "_" + n + ".wav"), sine (261.63, 44100.0, 4410));
    writeWav (dir.path / "samples" / "rel_C4.wav", sine (261.63, 44100.0, 100));

    writeText (dir.path / "Test Piano.sfz", R"(
        <control> default_path=samples/
        <global> amp_veltrack=100 ampeg_release=0.5
        <group> lovel=1 hivel=80
        <region> sample=soft_C4.wav lokey=58 hikey=62 pitch_keycenter=60
        <region> sample=soft_E4.wav lokey=63 hikey=68 pitch_keycenter=64
        <group> lovel=81 hivel=127 volume=-6
        <region> sample=loud_C4.wav lokey=58 hikey=62 pitch_keycenter=60
        <region> sample=loud_E4.wav lokey=63 hikey=68 pitch_keycenter=64 tune=50
        <group> trigger=release
        <region> sample=rel_C4.wav lokey=58 hikey=68
    )");

    std::string error;
    LoadReport report;
    std::atomic<float> progress { 0.0f };
    LoadOptions options;
    options.progress = &progress;
    const auto inst = load (dir.path / "Test Piano.sfz", error, &report, options);
    CK_CHECK (inst != nullptr);
    if (inst == nullptr) { std::printf ("    %s\n", error.c_str()); return; }

    CK_CHECK (inst->name == "Test Piano");
    CK_CHECK (inst->samples.size() == 4); // the release sample is skipped for now
    CK_CHECK (report.filesLoaded == 4);
    CK_CHECK (report.keysMapped == 11);
    CK_CHECK (progress.load() == 1.0f);
    CK_CHECK (! report.warnings.empty()); // mentions the skipped release region

    CK_CHECK (zoneFor (*inst, 57) == -1 && zoneFor (*inst, 69) == -1);
    CK_CHECK (zoneFor (*inst, 58) == zoneFor (*inst, 62));  // one zone per sampled note
    CK_CHECK (zoneFor (*inst, 62) != zoneFor (*inst, 63));
    CK_CHECK (inst->zones[size_t (zoneFor (*inst, 60))].rootKey == 60.0);
    CK_CHECK (inst->zones[size_t (zoneFor (*inst, 64))].rootKey == 64.0); // tune lives on the sample, so layers share the zone
    {
        const int softE = sampleFor (*inst, 64, 40), loudE = sampleFor (*inst, 64, 100);
        CK_CHECK (softE >= 0 && loudE >= 0 && softE != loudE);
        CK_CHECK (inst->samples[size_t (softE)].tuneCents == 0.0f);
        CK_CHECK (inst->samples[size_t (loudE)].tuneCents == -50.0f); // tune=50 raises pitch; the engine subtracts this
        CK_CHECK (std::abs (inst->samples[size_t (loudE)].releaseTimeConstant - 0.1f) < 1.0e-6f);
    }

    const int softC = sampleFor (*inst, 60, 40), loudC = sampleFor (*inst, 60, 100);
    CK_CHECK (softC >= 0 && loudC >= 0 && softC != loudC);
    CK_CHECK (sampleFor (*inst, 60, 80) == softC && sampleFor (*inst, 60, 81) == loudC);
    CK_CHECK (std::abs (inst->samples[size_t (loudC)].gain / inst->samples[size_t (softC)].gain - 0.5012f) < 0.001f); // -6 dB

    CK_CHECK (inst->velocityGain[127] == 1.0f && inst->velocityGain[64] < 0.3f);
    CK_CHECK (std::abs (inst->releaseTimeConstant - 0.1f) < 1.0e-6f);
}

CK_TEST ("sfz: loops come from the file or from opcodes, and loop_mode=no_loop turns them off")
{
    TempDir dir;
    writeWav (dir.path / "a.wav", sine (100.0, 44100.0, 5000), 1, 16, 1, 1000, 1999);
    writeText (dir.path / "loops.sfz",
        "<region> sample=a.wav key=60\n"
        "<region> sample=a.wav key=61 loop_mode=no_loop\n"
        "<region> sample=a.wav key=62 loop_mode=loop_sustain loop_start=2000 loop_end=3999\n");
    std::string error;
    const auto inst = load (dir.path / "loops.sfz", error);
    CK_CHECK (inst != nullptr);
    if (inst == nullptr) return;
    const auto& fromFile = inst->samples[size_t (sampleFor (*inst, 60, 100))];
    const auto& off = inst->samples[size_t (sampleFor (*inst, 61, 100))];
    const auto& fromOpcodes = inst->samples[size_t (sampleFor (*inst, 62, 100))];
    CK_CHECK (fromFile.loopStart == 1000 && fromFile.loopLength == 1000);
    CK_CHECK (! off.loops());
    CK_CHECK (fromOpcodes.loopStart == 2000 && fromOpcodes.loopLength == 2000);
}

CK_TEST ("sfz: pedal-down and round-robin regions don't replace the main ones")
{
    TempDir dir;
    for (const auto* n : { "up", "down", "rr2" })
        writeWav (dir.path / (std::string (n) + ".wav"), sine (200.0, 44100.0, 1000));
    writeText (dir.path / "p.sfz",
        "<region> sample=down.wav key=60 locc64=64 hicc64=127\n"
        "<region> sample=up.wav key=60 locc64=0 hicc64=63 seq_length=2 seq_position=1\n"
        "<region> sample=rr2.wav key=60 seq_length=2 seq_position=2\n");
    std::string error;
    LoadReport report;
    const auto inst = load (dir.path / "p.sfz", error, &report);
    CK_CHECK (inst != nullptr);
    if (inst == nullptr) return;
    CK_CHECK (inst->samples.size() == 1);
    CK_CHECK (report.warnings.size() == 2);
}

CK_TEST ("sfz: missing sample files are reported, the rest still load")
{
    TempDir dir;
    writeWav (dir.path / "here.wav", sine (200.0, 44100.0, 1000));
    writeText (dir.path / "m.sfz", "<region> sample=here.wav key=60\n<region> sample=gone.wav key=62\n");
    std::string error;
    LoadReport report;
    const auto inst = load (dir.path / "m.sfz", error, &report);
    CK_CHECK (inst != nullptr);
    CK_CHECK (report.filesLoaded == 1 && report.warnings.size() == 1);
    if (inst != nullptr)
        CK_CHECK (zoneFor (*inst, 62) == -1);

    writeText (dir.path / "none.sfz", "<region> sample=gone.wav key=62\n");
    CK_CHECK (load (dir.path / "none.sfz", error) == nullptr);
    CK_CHECK (! error.empty());
}

// ---------------------------------------------------------------------------
// File names and folders

CK_TEST ("names: notes, velocities, dynamics and round robins")
{
    auto check = [] (const char* stem, int note, int velocity, int dynamic = -1, int rr = 0, bool release = false)
    {
        const auto n = parseSampleName (stem);
        const bool ok = n.has_value() && n->note == note && n->velocity == velocity && n->dynamic == dynamic
                        && n->roundRobin == rr && n->release == release;
        if (! ok)
            std::printf ("    %s -> note %d vel %d dyn %d rr %d rel %d\n", stem, n ? n->note : -9, n ? n->velocity : -9,
                         n ? n->dynamic : -9, n ? n->roundRobin : -9, n ? int (n->release) : -9);
        CK_CHECK (ok);
    };

    check ("Grand_C4_v80", 60, 80);
    check ("Grand_C#3_v100_rr2", 49, 100, -1, 2);
    check ("piano-060-127", 60, 127);
    check ("Upright A0 ff", 21, -1, 6);
    check ("Bb2_vel64", 46, 64);
    check ("C4v3", 60, 3);
    check ("rel_D5", 74, -1, -1, 0, true);
    check ("Steinway C-1 layer2", 0, 2);
    check ("021_mf", 21, -1, 4);
    CK_CHECK (! parseSampleName ("ambience").has_value());
}

CK_TEST ("folder: velocity layers and key spread from file names")
{
    TempDir dir;
    for (int note : { 48, 60, 72 })
        for (int vel : { 40, 90, 127 })
            writeWav (dir.path / ("Keys_" + std::to_string (note) + "_v" + std::to_string (vel) + ".wav"), sine (100.0, 44100.0, 800));
    writeWav (dir.path / "Keys_060_v127_rr2.wav", sine (100.0, 44100.0, 800));
    writeText (dir.path / "readme.txt", "not audio");
    writeWav (dir.path / "._Keys_060_v40.wav", sine (100.0, 44100.0, 10)); // macOS resource fork junk

    std::string error;
    LoadReport report;
    const auto inst = load (dir.path, error, &report);
    CK_CHECK (inst != nullptr);
    if (inst == nullptr) { std::printf ("    %s\n", error.c_str()); return; }

    CK_CHECK (report.filesLoaded == 9);
    CK_CHECK (inst->zones.size() == 3);
    // Velocities in names are the top of each layer: 1-40, 41-90, 91-127.
    CK_CHECK (sampleFor (*inst, 60, 40) != sampleFor (*inst, 60, 41));
    CK_CHECK (sampleFor (*inst, 60, 41) == sampleFor (*inst, 60, 90));
    CK_CHECK (sampleFor (*inst, 60, 91) == sampleFor (*inst, 60, 127));
    // Keys go to the nearest sample, ties to the one below, and an octave past each end.
    CK_CHECK (inst->zones[size_t (zoneFor (*inst, 54))].rootKey == 48.0);
    CK_CHECK (inst->zones[size_t (zoneFor (*inst, 55))].rootKey == 60.0);
    CK_CHECK (zoneFor (*inst, 36) >= 0 && zoneFor (*inst, 35) == -1);
    CK_CHECK (zoneFor (*inst, 84) >= 0 && zoneFor (*inst, 85) == -1);
}

CK_TEST ("folder: dynamics in subfolder names become layers")
{
    TempDir dir;
    for (const auto* dyn : { "pp", "mf", "ff" })
        for (const auto* note : { "C3", "C5" })
            writeAiff (dir.path / dyn / (std::string ("Piano ") + note + ".aif"), sine (100.0, 44100.0, 500));

    std::string error;
    const auto inst = load (dir.path, error);
    CK_CHECK (inst != nullptr);
    if (inst == nullptr) { std::printf ("    %s\n", error.c_str()); return; }
    const int soft = sampleFor (*inst, 48, 10), mid = sampleFor (*inst, 48, 64), loud = sampleFor (*inst, 48, 127);
    CK_CHECK (soft >= 0 && mid >= 0 && loud >= 0 && soft != mid && mid != loud);
}

CK_TEST ("folder: C3-as-middle-C names are shifted up an octave")
{
    TempDir dir;
    for (const auto* note : { "A-1", "C3", "C7" }) // a full piano named with C3 = middle C
        writeWav (dir.path / (std::string (note) + ".wav"), sine (100.0, 44100.0, 500));
    std::string error;
    LoadReport report;
    const auto inst = load (dir.path, error, &report);
    CK_CHECK (inst != nullptr);
    if (inst == nullptr) return;
    CK_CHECK (inst->zones[size_t (zoneFor (*inst, 21))].rootKey == 21.0);
    CK_CHECK (inst->zones[size_t (zoneFor (*inst, 60))].rootKey == 60.0);
}

CK_TEST ("single file: plays across the keyboard at its unity note")
{
    TempDir dir;
    writeWav (dir.path / "pad.wav", sine (110.0, 44100.0, 2000), 1, 16, 1, -1, -1, 45);
    std::string error;
    const auto inst = load (dir.path / "pad.wav", error);
    CK_CHECK (inst != nullptr);
    if (inst == nullptr) return;
    CK_CHECK (zoneFor (*inst, 0) >= 0 && zoneFor (*inst, 127) >= 0);
    CK_CHECK (inst->zones[size_t (zoneFor (*inst, 60))].rootKey == 45.0);
}

CK_TEST ("loaders: canLoad and helpful errors")
{
    TempDir dir;
    writeText (dir.path / "x.sfz", "");
    CK_CHECK (canLoad (dir.path));
    CK_CHECK (canLoad (dir.path / "x.sfz"));
    CK_CHECK (canLoad ("y.FLAC"));
    CK_CHECK (! canLoad ("z.npno"));

    std::string error;
    CK_CHECK (load (dir.path / "x.sfz", error) == nullptr && ! error.empty());
    fs::create_directories (dir.path / "empty");
    CK_CHECK (load (dir.path / "empty", error) == nullptr && ! error.empty());

    std::atomic<bool> cancel { true };
    LoadOptions options;
    options.cancel = &cancel;
    writeWav (dir.path / "C4.wav", sine (100.0, 44100.0, 500));
    CK_CHECK (load (dir.path / "C4.wav", error, nullptr, options) == nullptr && error == "cancelled");
}

// ---------------------------------------------------------------------------
// End to end

CK_TEST ("an SFZ instrument plays through the engine, without allocating")
{
    TempDir dir;
    writeWav (dir.path / "a4.wav", sine (440.0, 48000.0, 48000), 1, 24, 1, -1, -1, -1, 48000.0);
    writeText (dir.path / "a.sfz", "<region> sample=a4.wav lokey=0 hikey=127 pitch_keycenter=69\n");
    std::string error;
    auto inst = load (dir.path / "a.sfz", error);
    CK_CHECK (inst != nullptr);
    if (inst == nullptr) return;

    ck::Engine engine;
    engine.prepare (48000.0);
    engine.setInstrument (inst);

    std::vector<float> l (512), r (512);
    float* out[] = { l.data(), r.data() };
    const std::vector<ck::MidiEvent> on { ck::MidiEvent::noteOn (0, 69, 110) };

    cktest::AllocationCounter::start();
    engine.process (out, 2, 512, on);
    for (int i = 0; i < 20; ++i)
        engine.process (out, 2, 512, {});
    const long allocations = cktest::AllocationCounter::stop();

    float peak = 0.0f;
    for (float v : l) peak = std::max (peak, std::abs (v));
    CK_CHECK (peak > 0.01f);
    CK_CHECK (allocations == 0);
}

CK_TEST ("sfz: velocities with no layer play the nearest one instead of silence")
{
    TempDir dir;
    writeWav (dir.path / "a.wav", sine (200.0, 44100.0, 500));
    writeWav (dir.path / "b.wav", sine (200.0, 44100.0, 500));
    writeText (dir.path / "gaps.sfz",
        "<region> sample=a.wav key=60 lovel=40 hivel=60\n"
        "<region> sample=b.wav key=60 lovel=90 hivel=127\n");
    std::string error;
    const auto inst = load (dir.path / "gaps.sfz", error);
    CK_CHECK (inst != nullptr);
    if (inst == nullptr) return;
    const int a = sampleFor (*inst, 60, 50), b = sampleFor (*inst, 60, 100);
    CK_CHECK (sampleFor (*inst, 60, 1) == a);  // below the lowest layer
    CK_CHECK (sampleFor (*inst, 60, 75) == a); // between layers: the one below
    CK_CHECK (sampleFor (*inst, 60, 127) == b);
}
