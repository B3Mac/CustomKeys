#include "customkeys/AudioFile.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO_WIDE
#if defined(__GNUC__)
 #pragma GCC diagnostic push
 #pragma GCC diagnostic ignored "-Wconversion"
 #pragma GCC diagnostic ignored "-Wsign-conversion"
 #pragma GCC diagnostic ignored "-Wshadow"
 #pragma GCC diagnostic ignored "-Wunused-function"
 #pragma GCC diagnostic ignored "-Wpedantic"
 #pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#include "../../third_party/dr_libs/dr_flac.h"
#if defined(__GNUC__)
 #pragma GCC diagnostic pop
#endif

namespace ck::audio {

namespace {

enum class Encoding { Int, Float };

struct Layout
{
    int channels = 0;
    double sampleRate = 0.0;
    int bits = 0;
    Encoding encoding = Encoding::Int;
    bool bigEndian = false;
    const uint8_t* data = nullptr;
    size_t dataBytes = 0;
};

uint16_t le16 (const uint8_t* p) { return static_cast<uint16_t> (p[0] | (p[1] << 8)); }
uint32_t le32 (const uint8_t* p) { return static_cast<uint32_t> (p[0] | (p[1] << 8) | (p[2] << 16)) | (static_cast<uint32_t> (p[3]) << 24); }
uint16_t be16 (const uint8_t* p) { return static_cast<uint16_t> ((p[0] << 8) | p[1]); }
uint32_t be32 (const uint8_t* p) { return (static_cast<uint32_t> (p[0]) << 24) | static_cast<uint32_t> ((p[1] << 16) | (p[2] << 8) | p[3]); }

bool readWholeFile (const std::filesystem::path& path, std::vector<uint8_t>& bytes, std::string& error)
{
    std::ifstream in (path, std::ios::binary);
    if (! in)
    {
        error = "can't open " + path.filename().string();
        return false;
    }
    in.seekg (0, std::ios::end);
    const auto size = in.tellg();
    if (size <= 0)
    {
        error = path.filename().string() + " is empty";
        return false;
    }
    in.seekg (0, std::ios::beg);
    bytes.resize (static_cast<size_t> (size));
    in.read (reinterpret_cast<char*> (bytes.data()), size);
    if (! in)
    {
        error = "couldn't read " + path.filename().string();
        return false;
    }
    return true;
}

/** Converts one stored sample to a float in -1..1. */
float decodeOne (const uint8_t* p, const Layout& layout)
{
    if (layout.encoding == Encoding::Float)
    {
        if (layout.bits == 32)
        {
            uint32_t raw = layout.bigEndian ? be32 (p) : le32 (p);
            float value;
            std::memcpy (&value, &raw, sizeof value);
            return value;
        }
        uint64_t raw = 0;
        for (int i = 0; i < 8; ++i)
            raw |= static_cast<uint64_t> (p[layout.bigEndian ? i : 7 - i]) << (8 * (7 - i));
        double value;
        std::memcpy (&value, &raw, sizeof value);
        return static_cast<float> (value);
    }

    const int bytes = layout.bits / 8;
    if (bytes == 1) // WAV stores 8-bit unsigned, AIFF signed
        return layout.bigEndian ? static_cast<float> (static_cast<int8_t> (p[0])) / 128.0f
                                : (static_cast<float> (p[0]) - 128.0f) / 128.0f;

    int64_t value = 0;
    for (int i = 0; i < bytes; ++i)
    {
        const uint8_t byte = layout.bigEndian ? p[i] : p[bytes - 1 - i];
        value = (value << 8) | byte;
    }
    const int shift = 64 - 8 * bytes; // sign-extend
    value = static_cast<int64_t> (static_cast<uint64_t> (value) << shift) >> shift;
    return static_cast<float> (static_cast<double> (value) / std::ldexp (1.0, 8 * bytes - 1));
}

int16_t toInt16 (float value)
{
    const float scaled = std::round (std::clamp (value, -1.0f, 1.0f) * 32767.0f);
    return static_cast<int16_t> (scaled);
}

bool fillSample (const Layout& layout, Sample& sample, FileInfo& info, std::string& error)
{
    if (layout.channels < 1 || layout.sampleRate <= 0.0)
    {
        error = "missing format information";
        return false;
    }
    const bool intOk = layout.encoding == Encoding::Int && (layout.bits == 8 || layout.bits == 16 || layout.bits == 24 || layout.bits == 32);
    const bool floatOk = layout.encoding == Encoding::Float && (layout.bits == 32 || layout.bits == 64);
    if (! intOk && ! floatOk)
    {
        error = "unsupported sample format (" + std::to_string (layout.bits) + "-bit)";
        return false;
    }

    const size_t frameBytes = static_cast<size_t> (layout.channels) * static_cast<size_t> (layout.bits / 8);
    const auto frames = static_cast<int64_t> (layout.dataBytes / frameBytes);
    if (frames <= 0)
    {
        error = "no audio data";
        return false;
    }

    const int keep = std::min (layout.channels, 2);
    sample.channels = keep;
    sample.frames = frames;
    sample.sampleRate = layout.sampleRate;
    sample.gain = 1.0f / 32767.0f;
    sample.data.resize (static_cast<size_t> (frames) * static_cast<size_t> (keep));

    const size_t sampleBytes = static_cast<size_t> (layout.bits / 8);
    for (int64_t f = 0; f < frames; ++f)
    {
        const uint8_t* frame = layout.data + static_cast<size_t> (f) * frameBytes;
        for (int c = 0; c < keep; ++c)
            sample.data[static_cast<size_t> (f) * static_cast<size_t> (keep) + static_cast<size_t> (c)]
                = toInt16 (decodeOne (frame + static_cast<size_t> (c) * sampleBytes, layout));
    }

    info.bitsPerSample = layout.bits;
    info.fileChannels = layout.channels;
    return true;
}

bool readWav (const std::vector<uint8_t>& bytes, Sample& sample, FileInfo& info, std::string& error)
{
    Layout layout;
    int64_t loopStart = -1, loopEnd = -1;

    size_t pos = 12;
    while (pos + 8 <= bytes.size())
    {
        const uint8_t* chunk = bytes.data() + pos;
        const size_t size = le32 (chunk + 4);
        const uint8_t* body = chunk + 8;
        const size_t available = std::min (size, bytes.size() - pos - 8);

        if (std::memcmp (chunk, "fmt ", 4) == 0 && available >= 16)
        {
            uint16_t format = le16 (body);
            layout.channels = le16 (body + 2);
            layout.sampleRate = le32 (body + 4);
            layout.bits = le16 (body + 14);
            if (format == 0xFFFE && available >= 26)
                format = le16 (body + 24); // first two bytes of the sub-format GUID
            if (format == 1)
                layout.encoding = Encoding::Int;
            else if (format == 3)
                layout.encoding = Encoding::Float;
            else
            {
                error = "compressed WAV (format " + std::to_string (format) + ") isn't supported";
                return false;
            }
        }
        else if (std::memcmp (chunk, "data", 4) == 0)
        {
            layout.data = body;
            layout.dataBytes = available;
        }
        else if (std::memcmp (chunk, "smpl", 4) == 0 && available >= 36)
        {
            info.unityNote = static_cast<int> (le32 (body + 12));
            info.unityCents = static_cast<double> (le32 (body + 16)) / 4294967296.0 * 100.0;
            const uint32_t loops = le32 (body + 28);
            if (loops > 0 && available >= 36 + 24)
            {
                loopStart = le32 (body + 36 + 8);
                loopEnd = le32 (body + 36 + 12); // inclusive
            }
        }

        pos += 8 + size + (size & 1);
    }

    if (layout.data == nullptr)
    {
        error = "no data chunk";
        return false;
    }
    if (! fillSample (layout, sample, info, error))
        return false;

    if (loopStart >= 0 && loopEnd > loopStart && loopEnd < sample.frames)
    {
        sample.loopStart = loopStart;
        sample.loopLength = loopEnd - loopStart + 1;
    }
    return true;
}

double readExtended80 (const uint8_t* p)
{
    const int exponent = ((p[0] & 0x7F) << 8) | p[1];
    uint64_t mantissa = 0;
    for (int i = 0; i < 8; ++i)
        mantissa = (mantissa << 8) | p[2 + i];
    if (exponent == 0 && mantissa == 0)
        return 0.0;
    const double value = std::ldexp (static_cast<double> (mantissa), exponent - 16383 - 63);
    return (p[0] & 0x80) ? -value : value;
}

bool readAiff (const std::vector<uint8_t>& bytes, Sample& sample, FileInfo& info, std::string& error)
{
    Layout layout;
    layout.bigEndian = true;
    const bool isAifc = std::memcmp (bytes.data() + 8, "AIFC", 4) == 0;

    size_t pos = 12;
    while (pos + 8 <= bytes.size())
    {
        const uint8_t* chunk = bytes.data() + pos;
        const size_t size = be32 (chunk + 4);
        const uint8_t* body = chunk + 8;
        const size_t available = std::min (size, bytes.size() - pos - 8);

        if (std::memcmp (chunk, "COMM", 4) == 0 && available >= 18)
        {
            layout.channels = be16 (body);
            layout.bits = be16 (body + 6);
            layout.sampleRate = readExtended80 (body + 8);
            if (isAifc && available >= 22)
            {
                const auto* type = body + 18;
                if (std::memcmp (type, "NONE", 4) == 0 || std::memcmp (type, "twos", 4) == 0)
                    layout.bigEndian = true;
                else if (std::memcmp (type, "sowt", 4) == 0)
                    layout.bigEndian = false;
                else if (std::memcmp (type, "fl32", 4) == 0 || std::memcmp (type, "FL32", 4) == 0)
                    layout.encoding = Encoding::Float, layout.bits = 32;
                else if (std::memcmp (type, "fl64", 4) == 0 || std::memcmp (type, "FL64", 4) == 0)
                    layout.encoding = Encoding::Float, layout.bits = 64;
                else
                {
                    error = "compressed AIFC (" + std::string (reinterpret_cast<const char*> (type), 4) + ") isn't supported";
                    return false;
                }
            }
            // AIFF stores the bit depth in whole bytes, rounded up (e.g. 20-bit in 3 bytes).
            if (layout.encoding == Encoding::Int)
                layout.bits = (layout.bits + 7) / 8 * 8;
        }
        else if (std::memcmp (chunk, "SSND", 4) == 0 && available >= 8)
        {
            const size_t offset = be32 (body);
            if (8 + offset <= available)
            {
                layout.data = body + 8 + offset;
                layout.dataBytes = available - 8 - offset;
            }
        }

        pos += 8 + size + (size & 1);
    }

    if (layout.data == nullptr)
    {
        error = "no SSND chunk";
        return false;
    }
    return fillSample (layout, sample, info, error);
}

bool readFlac (const std::filesystem::path& path, Sample& sample, FileInfo& info, std::string& error)
{
    unsigned int channels = 0, sampleRate = 0;
    drflac_uint64 frames = 0;
    drflac_int16* pcm = drflac_open_file_and_read_pcm_frames_s16 (path.string().c_str(), &channels, &sampleRate, &frames, nullptr);
    if (pcm == nullptr || frames == 0 || channels == 0)
    {
        if (pcm != nullptr)
            drflac_free (pcm, nullptr);
        error = "not a readable FLAC file";
        return false;
    }

    const int keep = static_cast<int> (std::min (channels, 2u));
    sample.channels = keep;
    sample.frames = static_cast<int64_t> (frames);
    sample.sampleRate = sampleRate;
    sample.gain = 1.0f / 32767.0f;
    sample.data.resize (static_cast<size_t> (frames) * static_cast<size_t> (keep));
    for (drflac_uint64 f = 0; f < frames; ++f)
        for (int c = 0; c < keep; ++c)
            sample.data[f * static_cast<size_t> (keep) + static_cast<size_t> (c)] = pcm[f * channels + static_cast<unsigned> (c)];
    drflac_free (pcm, nullptr);

    info.bitsPerSample = 16;
    info.fileChannels = static_cast<int> (channels);
    return true;
}

std::string lowerExtension (const std::filesystem::path& path)
{
    auto ext = path.extension().string();
    std::transform (ext.begin(), ext.end(), ext.begin(), [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });
    return ext;
}

} // namespace

bool isAudioFile (const std::filesystem::path& path)
{
    const auto ext = lowerExtension (path);
    return ext == ".wav" || ext == ".wave" || ext == ".aif" || ext == ".aiff" || ext == ".aifc" || ext == ".flac";
}

bool readAudioFile (const std::filesystem::path& path, Sample& sample, FileInfo& info, std::string& error)
{
    sample = Sample {};
    info = FileInfo {};

    std::error_code ec;
    if (! std::filesystem::is_regular_file (path, ec))
    {
        error = path.filename().string() + ": file not found";
        return false;
    }

    if (lowerExtension (path) == ".flac")
    {
        if (! readFlac (path, sample, info, error))
        {
            error = path.filename().string() + ": " + error;
            return false;
        }
        return true;
    }

    std::vector<uint8_t> bytes;
    if (! readWholeFile (path, bytes, error))
        return false;

    bool ok = false;
    if (bytes.size() >= 12 && std::memcmp (bytes.data(), "RIFF", 4) == 0 && std::memcmp (bytes.data() + 8, "WAVE", 4) == 0)
        ok = readWav (bytes, sample, info, error);
    else if (bytes.size() >= 12 && std::memcmp (bytes.data(), "FORM", 4) == 0
             && (std::memcmp (bytes.data() + 8, "AIFF", 4) == 0 || std::memcmp (bytes.data() + 8, "AIFC", 4) == 0))
        ok = readAiff (bytes, sample, info, error);
    else
        error = "not a WAV or AIFF file";

    if (! ok)
        error = path.filename().string() + ": " + error;
    return ok;
}

} // namespace ck::audio
