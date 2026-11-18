#include "customkeys/SampleLibrary.h"
#include "customkeys/AudioFile.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

namespace ck::library {

namespace fs = std::filesystem;

namespace {

std::string toLower (std::string_view s)
{
    std::string out (s);
    std::transform (out.begin(), out.end(), out.begin(), [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });
    return out;
}

bool cancelled (const LoadOptions& options)
{
    return options.cancel != nullptr && options.cancel->load();
}

void setProgress (const LoadOptions& options, float value)
{
    if (options.progress != nullptr)
        options.progress->store (value);
}

/** A sample placed on the keyboard: the common shape every format is reduced to. */
struct Placement
{
    int32_t sample = -1;
    int loKey = 0, hiKey = 127;
    int loVel = 1, hiVel = 127;
    double rootKey = 60.0;
};

/** Turns placements into the engine's key map: one zone per distinct
    (root, velocity table), looked up per key. Earlier placements win overlaps. */
void buildKeyMap (Instrument& instrument, const std::vector<Placement>& placements, LoadReport& report)
{
    instrument.zoneForKey.fill (-1);
    std::map<std::pair<double, std::array<int32_t, 128>>, int16_t> zoneIndex;
    bool warnedRoots = false;

    for (int key = 0; key < 128; ++key)
    {
        std::array<int32_t, 128> table;
        table.fill (-1);
        std::array<double, 128> roots {};
        bool any = false;

        for (int velocity = 1; velocity < 128; ++velocity)
            for (const auto& p : placements)
                if (key >= p.loKey && key <= p.hiKey && velocity >= p.loVel && velocity <= p.hiVel)
                {
                    table[static_cast<size_t> (velocity)] = p.sample;
                    roots[static_cast<size_t> (velocity)] = p.rootKey;
                    any = true;
                    break;
                }

        if (! any)
            continue;

        // Fill velocity gaps (from missing files or uneven layer ranges) with the layer
        // below, or the lowest layer for velocities under it, so no velocity is silent.
        int firstLayer = 1;
        while (table[static_cast<size_t> (firstLayer)] < 0) ++firstLayer;
        for (int v = 1; v < 128; ++v)
            if (table[static_cast<size_t> (v)] < 0)
            {
                const int from = v < firstLayer ? firstLayer : v - 1;
                table[static_cast<size_t> (v)] = table[static_cast<size_t> (from)];
                roots[static_cast<size_t> (v)] = roots[static_cast<size_t> (from)];
            }

        // A zone has one root. Layers recorded at different roots on the same key are
        // rare; use the root of a mid-loud layer and say so.
        int reference = 100;
        while (reference > 0 && table[static_cast<size_t> (reference)] < 0) --reference;
        if (reference == 0)
            for (reference = 101; reference < 127 && table[static_cast<size_t> (reference)] < 0; ++reference) {}
        const double root = roots[static_cast<size_t> (reference)];
        for (int v = 1; v < 128; ++v)
            if (table[static_cast<size_t> (v)] >= 0 && std::abs (roots[static_cast<size_t> (v)] - root) > 1.0e-6 && ! warnedRoots)
            {
                report.warnings.push_back ("some velocity layers on one key have different root notes; using one root per key");
                warnedRoots = true;
            }

        const auto signature = std::make_pair (root, table);
        auto it = zoneIndex.find (signature);
        if (it == zoneIndex.end())
        {
            Zone zone;
            zone.rootKey = root;
            zone.sampleForVelocity = table;
            instrument.zones.push_back (zone);
            it = zoneIndex.emplace (signature, static_cast<int16_t> (instrument.zones.size() - 1)).first;
        }
        instrument.zoneForKey[static_cast<size_t> (key)] = it->second;
        ++report.keysMapped;
    }
}

/** SFZ ampeg_release is the time to fade out; the engine wants a time constant
    (seconds to fall by 1/e). Five time constants is about -43 dB. */
float releaseTimeConstantFor (double ampegReleaseSeconds)
{
    return static_cast<float> (std::clamp (ampegReleaseSeconds / 5.0, 0.01, 2.0));
}

void setVelocityCurve (Instrument& instrument, double veltrackPercent)
{
    const double exponent = 2.0 * std::clamp (veltrackPercent, 0.0, 100.0) / 100.0;
    instrument.velocityGain[0] = 0.0f;
    for (int v = 1; v < 128; ++v)
        instrument.velocityGain[static_cast<size_t> (v)] = static_cast<float> (std::pow (v / 127.0, exponent));
}

// ---------------------------------------------------------------------------
// SFZ

std::shared_ptr<Instrument> loadSfzFile (const fs::path& path, std::string& error, LoadReport& report, const LoadOptions& options)
{
    std::ifstream in (path, std::ios::binary);
    if (! in)
    {
        error = "can't open " + path.filename().string();
        return nullptr;
    }
    std::stringstream text;
    text << in.rdbuf();

    const auto regions = parseSfz (text.str(), path.parent_path(), &report.warnings);

    // With key switches, play the articulation that is active before any switch is pressed.
    int keySwitch = -1;
    for (const auto& r : regions)
        if (r.swDefault >= 0) { keySwitch = r.swDefault; break; }
    if (keySwitch < 0)
        for (const auto& r : regions)
            if (r.swLast >= 0) { keySwitch = r.swLast; break; }

    // Keep the regions a piano plays with the pedal up, on the first round robin.
    std::vector<const SfzRegion*> playable;
    int releases = 0, alternates = 0, generators = 0, switched = 0, keyless = 0;
    for (const auto& r : regions)
    {
        if (r.swLast >= 0 && r.swLast != keySwitch) { ++switched; continue; }
        if (r.loKey < 0 || r.hiKey < r.loKey) { ++keyless; continue; }
        if (r.trigger == "release" || r.trigger == "release_key") { ++releases; continue; }
        if (r.seqPosition != 1 || r.loRand > 0.0) { ++alternates; continue; }
        if (r.sample.empty() || r.sample.string().front() == '*') { ++generators; continue; }
        playable.push_back (&r);
    }

    // Pedal-down sample sets (locc64=64..127) are used only for keys with no pedal-up set.
    std::vector<const SfzRegion*> pedalUp;
    std::copy_if (playable.begin(), playable.end(), std::back_inserter (pedalUp), [] (const SfzRegion* r) { return r->loCC64 <= 0; });
    if (pedalUp.size() < playable.size() && ! pedalUp.empty())
    {
        report.warnings.push_back (std::to_string (playable.size() - pedalUp.size()) + " pedal-down regions skipped (pedal-down sample sets come later)");
        playable = pedalUp;
    }

    if (releases > 0)   report.warnings.push_back (std::to_string (releases) + " release-trigger regions skipped (release sounds come later)");
    if (alternates > 0) report.warnings.push_back (std::to_string (alternates) + " round-robin alternates skipped; the first of each plays");
    if (generators > 0) report.warnings.push_back (std::to_string (generators) + " regions without a sample file skipped");
    if (switched > 0)   report.warnings.push_back (std::to_string (switched) + " regions on other key switches skipped; the default articulation plays");
    if (keyless > 0)    report.warnings.push_back (std::to_string (keyless) + " controller-triggered regions (like pedal noise) skipped");

    if (playable.empty())
    {
        error = path.filename().string() + " has no playable regions";
        return nullptr;
    }

    auto instrument = std::make_shared<Instrument>();
    instrument->name = path.stem().string();

    // One Sample per distinct (file, volume, loop, tune, release) so each region keeps its own
    // level, loop, fine tune and key-up fade.
    using Variant = std::tuple<fs::path, double, std::string, int64_t, int64_t, double, double>;
    const auto variantOf = [] (const SfzRegion& r)
    {
        return Variant { r.sample, r.volumeDb, r.loopMode, r.loopStart, r.loopEnd, r.tuneCents, r.ampegRelease };
    };
    std::map<Variant, int32_t> variantIndex;
    std::map<fs::path, std::vector<Variant>> variantsByFile;
    for (const auto* r : playable)
    {
        const Variant v = variantOf (*r);
        if (variantIndex.emplace (v, -1).second)
            variantsByFile[r->sample].push_back (v);
    }

    std::map<fs::path, audio::FileInfo> infoByFile;
    size_t done = 0;
    int missing = 0;
    for (auto& [file, variants] : variantsByFile)
    {
        if (cancelled (options))
        {
            error = "cancelled";
            return nullptr;
        }

        Sample base;
        audio::FileInfo info;
        std::string fileError;
        if (! audio::readAudioFile (file, base, info, fileError))
        {
            if (++missing <= 5)
                report.warnings.push_back (fileError);
            setProgress (options, static_cast<float> (++done) / static_cast<float> (variantsByFile.size()));
            continue;
        }
        infoByFile[file] = info;
        ++report.filesLoaded;

        for (size_t i = 0; i < variants.size(); ++i)
        {
            const auto& [p, volumeDb, loopMode, loopStart, loopEnd, tuneCents, ampegRelease] = variants[i];
            Sample s = (i + 1 == variants.size()) ? std::move (base) : base;
            s.gain *= static_cast<float> (std::pow (10.0, volumeDb / 20.0));
            s.tuneCents = static_cast<float> (-tuneCents); // the engine subtracts it; tune=+10 plays sharper
            s.releaseTimeConstant = releaseTimeConstantFor (ampegRelease);

            const bool fileLoops = s.loopLength > 0;
            const bool wantLoop = loopMode == "loop_continuous" || loopMode == "loop_sustain" || (loopMode.empty() && fileLoops);
            if (! wantLoop)
                s.loopStart = s.loopLength = 0;
            else if (loopStart >= 0 && loopEnd > loopStart && loopEnd < s.frames)
            {
                s.loopStart = loopStart;
                s.loopLength = loopEnd - loopStart + 1;
            }

            instrument->samples.push_back (std::move (s));
            variantIndex[variants[i]] = static_cast<int32_t> (instrument->samples.size() - 1);
        }
        setProgress (options, static_cast<float> (++done) / static_cast<float> (variantsByFile.size()));
    }
    if (missing > 5)
        report.warnings.push_back (std::to_string (missing - 5) + " more sample files couldn't be read");

    std::vector<Placement> placements;
    for (const auto* r : playable)
    {
        const int32_t index = variantIndex[variantOf (*r)];
        if (index < 0)
            continue;

        double center = r->keyCenter;
        if (r->keyCenterFromSample)
        {
            const auto& info = infoByFile[r->sample];
            center = info.unityNote >= 0 ? info.unityNote + info.unityCents / 100.0 : 60.0;
        }
        // Playing key k sounds at (k - center + transpose) semitones from the recording, plus the
        // recording's fine tune, which lives on the Sample so layers with different tunes share a zone.
        placements.push_back ({ index, r->loKey, r->hiKey, std::max (r->loVel, 1), r->hiVel,
                                center - r->transpose });
    }

    if (instrument->samples.empty())
    {
        error = "none of the sample files in " + path.filename().string() + " could be read";
        return nullptr;
    }

    buildKeyMap (*instrument, placements, report);
    setVelocityCurve (*instrument, playable.front()->ampVeltrack);
    instrument->releaseTimeConstant = releaseTimeConstantFor (playable.front()->ampegRelease);
    return instrument;
}

// ---------------------------------------------------------------------------
// Folders and single files

struct NamedFile
{
    fs::path path;
    SampleName name;
};

std::vector<fs::path> listAudioFiles (const fs::path& folder)
{
    std::vector<fs::path> files;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator (folder, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment (ec))
    {
        if (ec)
            break;
        if (it.depth() > 3)
        {
            it.disable_recursion_pending();
            continue;
        }
        const auto filename = it->path().filename().string();
        if (! filename.empty() && filename.front() == '.')
            continue; // hidden files, including macOS "._" resource forks
        if (it->is_regular_file (ec) && audio::isAudioFile (it->path()))
            files.push_back (it->path());
    }
    std::sort (files.begin(), files.end());
    return files;
}

std::shared_ptr<Instrument> loadSampleFiles (const std::vector<fs::path>& files, const std::string& name,
                                             std::string& error, LoadReport& report, const LoadOptions& options)
{
    if (files.empty())
    {
        error = "no WAV, AIFF or FLAC files found";
        return nullptr;
    }

    auto instrument = std::make_shared<Instrument>();
    instrument->name = name;

    struct Loaded
    {
        int32_t sample;
        SampleName name;
        double cents;
    };
    std::vector<Loaded> loaded;
    int unnamed = 0, releases = 0, alternates = 0, unreadable = 0;

    for (size_t i = 0; i < files.size(); ++i)
    {
        if (cancelled (options))
        {
            error = "cancelled";
            return nullptr;
        }
        setProgress (options, static_cast<float> (i) / static_cast<float> (files.size()));

        auto parsed = parseSampleName (files[i].stem().string()).value_or (SampleName {});

        // Velocity or dynamics can also sit in a parent folder name, e.g. "ff/C4.wav".
        if (parsed.velocity < 0 && parsed.dynamic < 0 && files[i].has_parent_path())
            if (auto folderName = parseSampleName ("x_" + files[i].parent_path().filename().string()))
            {
                parsed.velocity = folderName->velocity;
                parsed.dynamic = folderName->dynamic;
            }

        if (parsed.release) { ++releases; continue; }
        if (parsed.roundRobin > 1) { ++alternates; continue; }

        Sample sample;
        audio::FileInfo info;
        std::string fileError;
        if (! audio::readAudioFile (files[i], sample, info, fileError))
        {
            if (++unreadable <= 5)
                report.warnings.push_back (fileError);
            continue;
        }

        double cents = 0.0;
        if (parsed.note < 0 && info.unityNote >= 0 && info.unityNote < 128)
        {
            parsed.note = info.unityNote;
            cents = info.unityCents;
        }
        if (parsed.note < 0 && files.size() > 1)
        {
            ++unnamed;
            continue;
        }

        instrument->samples.push_back (std::move (sample));
        loaded.push_back ({ static_cast<int32_t> (instrument->samples.size() - 1), parsed, cents });
        ++report.filesLoaded;
    }
    setProgress (options, 1.0f);

    if (releases > 0)   report.warnings.push_back (std::to_string (releases) + " release samples skipped (release sounds come later)");
    if (alternates > 0) report.warnings.push_back (std::to_string (alternates) + " round-robin alternates skipped; the first of each plays");
    if (unnamed > 0)    report.warnings.push_back (std::to_string (unnamed) + " files skipped: no note in the name (like C4 or 060)");
    if (unreadable > 5) report.warnings.push_back (std::to_string (unreadable - 5) + " more files couldn't be read");

    if (loaded.empty())
    {
        error = "no sample names gave a note (expected names like Piano_C4_v80.wav)";
        return nullptr;
    }

    // A single recording with no note plays at its own pitch on middle C and covers the keyboard.
    if (loaded.size() == 1)
    {
        auto& only = loaded.front();
        const double root = (only.name.note >= 0 ? only.name.note : 60) + only.cents / 100.0;
        std::vector<Placement> placements { { only.sample, 0, 127, 1, 127, root } };
        buildKeyMap (*instrument, placements, report);
        setVelocityCurve (*instrument, 100.0);
        return instrument;
    }

    // Names with C3 as middle C put a piano's bottom A at A-1. Shift them up an octave.
    int lowest = 127, highest = 0;
    for (const auto& l : loaded) { lowest = std::min (lowest, l.name.note); highest = std::max (highest, l.name.note); }
    if (lowest < 21 && highest + 12 <= 108 && highest - lowest >= 24)
    {
        for (auto& l : loaded) l.name.note += 12;
        lowest += 12; highest += 12;
        report.warnings.push_back ("note names look like C3 = middle C; shifted up an octave");
    }

    // Velocity layers per note.
    std::map<int, std::vector<const Loaded*>> byNote;
    for (const auto& l : loaded)
        byNote[l.name.note].push_back (&l);

    std::set<int> velocityValues;
    bool anyDynamic = false;
    for (const auto& l : loaded)
    {
        if (l.name.velocity >= 0) velocityValues.insert (l.name.velocity);
        if (l.name.dynamic >= 0) anyDynamic = true;
    }
    const bool velocitiesAreIndices = ! velocityValues.empty() && *velocityValues.rbegin() <= 16;
    const bool velocitiesAreLowerBounds = ! velocityValues.empty() && *velocityValues.begin() <= 1 && ! velocitiesAreIndices;

    std::vector<Placement> placements;
    int duplicates = 0;
    for (auto& [note, layers] : byNote)
    {
        auto rank = [&] (const Loaded* l)
        {
            if (! velocityValues.empty() && l->name.velocity >= 0) return l->name.velocity;
            if (anyDynamic && l->name.dynamic >= 0) return l->name.dynamic;
            return 0;
        };
        std::stable_sort (layers.begin(), layers.end(), [&] (const Loaded* a, const Loaded* b) { return rank (a) < rank (b); });

        // Same note and same layer twice: keep the first.
        std::vector<const Loaded*> unique;
        for (const auto* l : layers)
            if (unique.empty() || rank (unique.back()) != rank (l))
                unique.push_back (l);
            else
                ++duplicates;

        const auto count = static_cast<int> (unique.size());
        for (int i = 0; i < count; ++i)
        {
            int lo, hi;
            const bool explicitVelocity = ! velocityValues.empty() && ! velocitiesAreIndices && unique[static_cast<size_t> (i)]->name.velocity >= 0;
            if (explicitVelocity && velocitiesAreLowerBounds)
            {
                lo = std::max (1, rank (unique[static_cast<size_t> (i)]));
                hi = i + 1 < count ? rank (unique[static_cast<size_t> (i + 1)]) - 1 : 127;
            }
            else if (explicitVelocity)
            {
                lo = i == 0 ? 1 : rank (unique[static_cast<size_t> (i - 1)]) + 1;
                hi = i + 1 < count ? rank (unique[static_cast<size_t> (i)]) : 127;
            }
            else // layer numbers or dynamics: split the velocity range evenly
            {
                lo = i * 127 / count + 1;
                hi = (i + 1) * 127 / count;
            }
            placements.push_back ({ unique[static_cast<size_t> (i)]->sample, note, note, lo, std::max (lo, hi),
                                    note + unique[static_cast<size_t> (i)]->cents / 100.0 });
        }
    }
    if (duplicates > 0)
        report.warnings.push_back (std::to_string (duplicates) + " files share a note and layer with another; the first plays");

    // Spread each sampled note over the keys nearest to it (ties go to the note below),
    // reaching up to an octave past the lowest and highest samples.
    std::vector<int> notes;
    for (const auto& [note, layers] : byNote) notes.push_back (note);
    for (auto& p : placements)
    {
        const auto at = std::lower_bound (notes.begin(), notes.end(), p.loKey);
        const int below = at == notes.begin() ? -1 : *(at - 1);
        const int above = at + 1 == notes.end() ? -1 : *(at + 1);
        p.loKey = below < 0 ? std::max (0, p.loKey - 12) : (below + p.loKey) / 2 + 1;
        p.hiKey = above < 0 ? std::min (127, p.hiKey + 12) : (p.hiKey + above) / 2;
    }

    buildKeyMap (*instrument, placements, report);
    setVelocityCurve (*instrument, velocityValues.empty() && ! anyDynamic ? 100.0 : 30.0);
    return instrument;
}

std::string lowerExtension (const fs::path& path)
{
    return toLower (path.extension().string());
}

/** "v80" with prefixes {"v"} gives 80; -1 when the token isn't prefix + digits. */
int numberAfterPrefix (const std::string& token, std::initializer_list<const char*> prefixes)
{
    for (const char* prefix : prefixes)
    {
        const std::string p (prefix);
        if (token.size() > p.size() && token.size() <= p.size() + 3 && token.rfind (p, 0) == 0
            && std::all_of (token.begin() + static_cast<std::ptrdiff_t> (p.size()), token.end(), ::isdigit))
            return std::stoi (token.substr (p.size()));
    }
    return -1;
}

} // namespace

std::optional<SampleName> parseSampleName (std::string_view stem)
{
    // Split on separators, keeping '#' and '-' inside tokens like "C#4" and "C-1".
    std::vector<std::string> tokens;
    std::string current;
    for (size_t i = 0; i < stem.size(); ++i)
    {
        const char c = stem[i];
        // "C-1": a minus right after a bare note letter is part of the octave.
        const bool afterNoteLetter = (current.size() == 1 || (current.size() == 2 && (current[1] == '#' || current[1] == 'b')))
                                     && std::strchr ("abcdefgABCDEFG", current[0]) != nullptr;
        const bool minusSign = c == '-' && afterNoteLetter && i + 1 < stem.size()
                               && std::isdigit (static_cast<unsigned char> (stem[i + 1]));
        if (std::isalnum (static_cast<unsigned char> (c)) || c == '#' || minusSign)
            current += c;
        else if (! current.empty())
        {
            tokens.push_back (current);
            current.clear();
        }
    }
    if (! current.empty())
        tokens.push_back (current);

    static const std::map<std::string, int> dynamics { { "ppp", 0 }, { "pp", 1 }, { "p", 2 }, { "mp", 3 },
                                                       { "mf", 4 }, { "f", 5 }, { "ff", 6 }, { "fff", 7 } };

    SampleName name;
    std::vector<int> plainNumbers;
    bool found = false;

    for (const auto& token : tokens)
    {
        const auto t = toLower (token);

        if (t == "rel" || t == "release" || t == "rls")
        {
            name.release = true;
            found = true;
            continue;
        }
        if (auto d = dynamics.find (t); d != dynamics.end())
        {
            name.dynamic = d->second;
            found = true;
            continue;
        }

        // rr2, rr_2 (the number may be its own token after "rr")
        if (t.rfind ("rr", 0) == 0 && t.size() > 2 && std::all_of (t.begin() + 2, t.end(), ::isdigit))
        {
            name.roundRobin = std::stoi (t.substr (2));
            found = true;
            continue;
        }

        // v80, vel80, velocity80, l3, layer3
        if (const int velocity = numberAfterPrefix (t, { "velocity", "vel", "v", "layer", "l" }); velocity >= 0)
        {
            name.velocity = velocity;
            found = true;
            continue;
        }

        // A note name, optionally fused with a velocity: C4, F#2, Bb3, C4v80
        if (name.note < 0)
        {
            const auto v = t.find ('v', 1);
            const std::string noteText = v == std::string::npos ? t : t.substr (0, v);
            const int note = parseSfzKey (noteText);
            if (note >= 0 && ! std::isdigit (static_cast<unsigned char> (noteText.front())))
            {
                name.note = note;
                if (v != std::string::npos && v + 1 < t.size() && std::all_of (t.begin() + static_cast<std::ptrdiff_t> (v) + 1, t.end(), ::isdigit))
                    name.velocity = std::stoi (t.substr (v + 1));
                found = true;
                continue;
            }
        }

        if (std::all_of (t.begin(), t.end(), ::isdigit) && t.size() <= 3)
            plainNumbers.push_back (std::stoi (t));
    }

    // Plain numbers: the first is the MIDI note when no note name was found, the next a velocity.
    size_t next = 0;
    if (name.note < 0 && next < plainNumbers.size() && plainNumbers[next] <= 127)
    {
        name.note = plainNumbers[next++];
        found = true;
    }
    if (name.velocity < 0 && name.note >= 0 && next < plainNumbers.size() && plainNumbers[next] <= 127)
    {
        name.velocity = plainNumbers[next];
        found = true;
    }

    if (! found)
        return std::nullopt;
    return name;
}

bool canLoad (const fs::path& path)
{
    std::error_code ec;
    if (fs::is_directory (path, ec))
        return true;
    return lowerExtension (path) == ".sfz" || audio::isAudioFile (path);
}

std::shared_ptr<Instrument> load (const fs::path& path, std::string& error, LoadReport* report, const LoadOptions& options)
{
    LoadReport local;
    LoadReport& r = report != nullptr ? *report : local;
    r = LoadReport {};
    setProgress (options, 0.0f);

    std::error_code ec;
    if (fs::is_directory (path, ec))
    {
        auto name = path.filename().string();
        if (name.empty())
            name = path.parent_path().filename().string();
        return loadSampleFiles (listAudioFiles (path), name, error, r, options);
    }
    if (! fs::exists (path, ec))
    {
        error = path.filename().string() + " doesn't exist";
        return nullptr;
    }
    if (lowerExtension (path) == ".sfz")
        return loadSfzFile (path, error, r, options);
    if (audio::isAudioFile (path))
        return loadSampleFiles ({ path }, path.stem().string(), error, r, options);

    error = path.filename().string() + " isn't an SFZ, WAV, AIFF or FLAC file";
    return nullptr;
}

} // namespace ck::library
