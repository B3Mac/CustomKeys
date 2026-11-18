#include "customkeys/SampleLibrary.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>

namespace ck::library {

namespace {

using Opcodes = std::map<std::string, std::string>;

std::string toLower (std::string_view s)
{
    std::string out (s);
    std::transform (out.begin(), out.end(), out.begin(), [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });
    return out;
}

std::string_view trim (std::string_view s)
{
    while (! s.empty() && std::isspace (static_cast<unsigned char> (s.front()))) s.remove_prefix (1);
    while (! s.empty() && std::isspace (static_cast<unsigned char> (s.back())))  s.remove_suffix (1);
    return s;
}

bool isNameChar (char c) { return std::isalnum (static_cast<unsigned char> (c)) || c == '_'; }

double toDouble (const std::string& s, double fallback)
{
    try { size_t used = 0; const double v = std::stod (s, &used); return used > 0 ? v : fallback; }
    catch (...) { return fallback; }
}

int toInt (const std::string& s, int fallback)
{
    return static_cast<int> (std::lround (toDouble (s, fallback)));
}

std::string readFile (const std::filesystem::path& path)
{
    std::ifstream in (path, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

/** Replaces $macros, longest names first so $A does not eat $AB. */
std::string substitute (std::string text, const std::map<std::string, std::string>& defines)
{
    if (text.find ('$') == std::string::npos)
        return text;
    std::vector<std::pair<std::string, std::string>> ordered (defines.begin(), defines.end());
    std::sort (ordered.begin(), ordered.end(), [] (const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
    for (const auto& [name, value] : ordered)
        for (size_t at = text.find (name); at != std::string::npos; at = text.find (name, at + value.size()))
            text.replace (at, name.size(), value);
    return text;
}

/** Removes comments and expands #include and #define. */
std::string preprocess (std::string_view text, const std::filesystem::path& folder,
                        std::map<std::string, std::string>& defines, int depth,
                        std::vector<std::string>* warnings)
{
    // Strip // line comments and /* block */ comments.
    std::string clean;
    clean.reserve (text.size());
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '/')
        {
            while (i < text.size() && text[i] != '\n') ++i;
            clean += '\n';
        }
        else if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '*')
        {
            const auto end = text.find ("*/", i + 2);
            i = end == std::string_view::npos ? text.size() : end + 1;
            clean += ' ';
        }
        else
            clean += text[i];
    }

    std::string out;
    std::istringstream lines (clean);
    std::string line;
    while (std::getline (lines, line))
    {
        const auto trimmed = trim (line);
        if (trimmed.rfind ("#define", 0) == 0)
        {
            std::istringstream parts { std::string (trimmed.substr (7)) };
            std::string name, value;
            parts >> name;
            std::getline (parts, value);
            if (! name.empty())
                defines[name] = std::string (trim (value));
            continue;
        }

        // #include can sit anywhere in a line, e.g. "<group> #include "a.txt" lovel=1".
        // Pieces are handled left to right so a #define from one include applies to the next.
        std::string_view rest = line;
        while (true)
        {
            const auto at = rest.find ("#include");
            out += substitute (std::string (rest.substr (0, at)), defines);
            if (at == std::string_view::npos)
                break;

            const auto open = rest.find ('"', at);
            const auto close = open == std::string_view::npos ? open : rest.find ('"', open + 1);
            if (close == std::string_view::npos)
                break;

            const auto relative = std::string (rest.substr (open + 1, close - open - 1));
            const auto path = folder / relative;
            if (depth >= 8)
            {
                if (warnings) warnings->push_back ("#include nested too deeply: " + relative);
            }
            else if (! std::filesystem::exists (path))
            {
                if (warnings) warnings->push_back ("missing #include: " + relative);
            }
            else
                out += "\n" + preprocess (readFile (path), folder, defines, depth + 1, warnings) + "\n";

            rest = rest.substr (close + 1);
        }
        out += '\n';
    }
    return out;
}

/** Sum of `prefix<N>=amount` opcodes, each scaled by controller N's starting value
    from <control> set_ccN (0-127) or set_hdccN (0-1). Controllers never set start at 0. */
double controllerAmount (const Opcodes& ops, const Opcodes& control, const std::string& prefix)
{
    double total = 0.0;
    for (auto it = ops.lower_bound (prefix); it != ops.end() && it->first.rfind (prefix, 0) == 0; ++it)
    {
        const auto cc = it->first.substr (prefix.size());
        double value = 0.0;
        if (auto hd = control.find ("set_hdcc" + cc); hd != control.end())
            value = toDouble (hd->second, 0.0);
        else if (auto lo = control.find ("set_cc" + cc); lo != control.end())
            value = toDouble (lo->second, 0.0) / 127.0;
        total += toDouble (it->second, 0.0) * value;
    }
    return total;
}

SfzRegion toRegion (const Opcodes& ops, const std::filesystem::path& folder, const Opcodes& control)
{
    auto get = [&] (std::initializer_list<const char*> names) -> const std::string*
    {
        for (const char* n : names)
            if (auto it = ops.find (n); it != ops.end())
                return &it->second;
        return nullptr;
    };

    int noteOffset = 0;
    if (auto it = control.find ("note_offset"); it != control.end())   noteOffset += toInt (it->second, 0);
    if (auto it = control.find ("octave_offset"); it != control.end()) noteOffset += 12 * toInt (it->second, 0);
    auto key = [&] (const std::string& v, int fallback)
    {
        const int k = parseSfzKey (v);
        return k < 0 ? fallback : std::clamp (k + noteOffset, 0, 127);
    };

    SfzRegion r;
    if (auto* v = get ({ "sample" }))
    {
        std::string path = *v;
        std::replace (path.begin(), path.end(), '\\', '/');
        if (! path.empty() && path.front() != '*')
        {
            std::string prefix;
            if (auto it = control.find ("default_path"); it != control.end())
            {
                prefix = it->second;
                std::replace (prefix.begin(), prefix.end(), '\\', '/');
            }
            r.sample = (folder / (prefix + path)).lexically_normal();
        }
        else
            r.sample = path; // a generator like *sine; skipped when building
    }

    if (auto* v = get ({ "key" }))
    {
        r.loKey = r.hiKey = key (*v, 60);
        r.keyCenter = r.loKey;
    }
    if (auto* v = get ({ "lokey" }))   r.loKey = key (*v, r.loKey);
    if (auto* v = get ({ "hikey" }))   r.hiKey = key (*v, r.hiKey);
    for (const char* name : { "key", "lokey", "hikey" })
        if (auto* v = get ({ name }); v != nullptr && trim (*v) == "-1")
            r.loKey = r.hiKey = -1; // played by a controller, not a key (pedal noise and the like)
    if (auto* v = get ({ "sw_last" }))    r.swLast = key (*v, -1);
    if (auto* v = get ({ "sw_default" })) r.swDefault = key (*v, -1);
    if (auto* v = get ({ "pitch_keycenter" }))
    {
        if (toLower (*v) == "sample")
            r.keyCenterFromSample = true;
        else
            r.keyCenter = key (*v, static_cast<int> (r.keyCenter));
    }
    if (auto* v = get ({ "lovel" }))   r.loVel = std::clamp (toInt (*v, 1), 0, 127);
    if (auto* v = get ({ "hivel" }))   r.hiVel = std::clamp (toInt (*v, 127), 0, 127);
    if (auto* v = get ({ "tune", "pitch" }))      r.tuneCents = toDouble (*v, 0.0);
    if (auto* v = get ({ "transpose" }))          r.transpose = toInt (*v, 0);
    if (auto* v = get ({ "volume", "gain" }))     r.volumeDb = toDouble (*v, 0.0);
    if (auto* v = get ({ "amp_veltrack" }))       r.ampVeltrack = toDouble (*v, 100.0);
    if (auto* v = get ({ "ampeg_release" }))      r.ampegRelease = toDouble (*v, 0.001);
    r.ampVeltrack = std::clamp (r.ampVeltrack + controllerAmount (ops, control, "amp_veltrack_oncc"), -100.0, 100.0);
    r.ampegRelease = std::max (0.0, r.ampegRelease + controllerAmount (ops, control, "ampeg_release_oncc"));
    if (auto* v = get ({ "trigger" }))            r.trigger = toLower (*v);
    if (auto* v = get ({ "loop_mode", "loopmode" })) r.loopMode = toLower (*v);
    if (auto* v = get ({ "loop_start", "loopstart" })) r.loopStart = static_cast<int64_t> (toDouble (*v, -1.0));
    if (auto* v = get ({ "loop_end", "loopend" }))     r.loopEnd = static_cast<int64_t> (toDouble (*v, -1.0));
    if (auto* v = get ({ "seq_position" }))       r.seqPosition = toInt (*v, 1);
    if (auto* v = get ({ "lorand" }))             r.loRand = toDouble (*v, 0.0);
    if (auto* v = get ({ "hirand" }))             r.hiRand = toDouble (*v, 1.0);
    if (auto* v = get ({ "locc64" }))             r.loCC64 = toInt (*v, 0);
    if (auto* v = get ({ "hicc64" }))             r.hiCC64 = toInt (*v, 127);
    return r;
}

} // namespace

int parseSfzKey (std::string_view value)
{
    const auto s = trim (value);
    if (s.empty())
        return -1;

    if (std::isdigit (static_cast<unsigned char> (s.front())) || s.front() == '-')
    {
        int n = -1;
        const auto [end, ec] = std::from_chars (s.data(), s.data() + s.size(), n);
        return (ec == std::errc() && end == s.data() + s.size() && n >= 0 && n <= 127) ? n : -1;
    }

    static constexpr int semitone[] = { 9, 11, 0, 2, 4, 5, 7 }; // a b c d e f g
    const char letter = static_cast<char> (std::tolower (static_cast<unsigned char> (s.front())));
    if (letter < 'a' || letter > 'g')
        return -1;

    size_t i = 1;
    int note = semitone[letter - 'a'];
    if (i < s.size() && (s[i] == '#' || s[i] == 's')) { ++note; ++i; }
    else if (i < s.size() && s[i] == 'b' && i + 1 < s.size()) { --note; ++i; }

    int octave = 0;
    const auto [end, ec] = std::from_chars (s.data() + i, s.data() + s.size(), octave);
    if (ec != std::errc() || end != s.data() + s.size())
        return -1;

    const int midi = (octave + 1) * 12 + note;
    return midi >= 0 && midi <= 127 ? midi : -1;
}

std::vector<SfzRegion> parseSfz (std::string_view text, const std::filesystem::path& sfzFolder,
                                 std::vector<std::string>* warnings)
{
    std::map<std::string, std::string> defines;
    const std::string source = preprocess (text, sfzFolder, defines, 0, warnings);

    enum class Level { None, Control, Global, Master, Group, Region, Other };
    Level level = Level::None;
    Opcodes control, global, master, group, region;
    std::vector<SfzRegion> regions;

    auto finishRegion = [&]
    {
        if (level != Level::Region)
            return;
        Opcodes merged = global;
        for (const auto* layer : { &master, &group, &region })
            for (const auto& [k, v] : *layer)
                merged[k] = v;
        regions.push_back (toRegion (merged, sfzFolder, control));
        region.clear();
    };

    auto target = [&]() -> Opcodes*
    {
        switch (level)
        {
            case Level::Control: return &control;
            case Level::Global:  return &global;
            case Level::Master:  return &master;
            case Level::Group:   return &group;
            case Level::Region:  return &region;
            default:             return nullptr;
        }
    };

    const size_t n = source.size();
    size_t i = 0;
    while (i < n)
    {
        if (std::isspace (static_cast<unsigned char> (source[i]))) { ++i; continue; }

        if (source[i] == '<')
        {
            const auto close = source.find ('>', i);
            if (close == std::string::npos)
                break;
            const auto header = toLower (trim (std::string_view (source).substr (i + 1, close - i - 1)));
            finishRegion();

            if (header == "control")     { level = Level::Control; control.clear(); }
            else if (header == "global") { level = Level::Global; global.clear(); master.clear(); group.clear(); }
            else if (header == "master") { level = Level::Master; master.clear(); group.clear(); }
            else if (header == "group")  { level = Level::Group; group.clear(); }
            else if (header == "region") { level = Level::Region; region.clear(); }
            else                         { level = Level::Other; }

            i = close + 1;
            continue;
        }

        // opcode=value. The value runs to the end of the line, the next header,
        // or the next "name=" after whitespace, so sample paths may hold spaces.
        size_t nameEnd = i;
        while (nameEnd < n && isNameChar (source[nameEnd])) ++nameEnd;
        if (nameEnd == i || nameEnd >= n || source[nameEnd] != '=')
        {
            while (i < n && ! std::isspace (static_cast<unsigned char> (source[i])) && source[i] != '<') ++i;
            if (i == nameEnd) ++i;
            continue;
        }

        const auto name = toLower (std::string_view (source).substr (i, nameEnd - i));
        size_t valueEnd = nameEnd + 1;
        while (valueEnd < n && source[valueEnd] != '\n' && source[valueEnd] != '\r' && source[valueEnd] != '<')
        {
            if (std::isspace (static_cast<unsigned char> (source[valueEnd])))
            {
                size_t look = valueEnd;
                while (look < n && (source[look] == ' ' || source[look] == '\t')) ++look;
                size_t word = look;
                while (word < n && isNameChar (source[word])) ++word;
                if (word > look && word < n && source[word] == '=')
                    break;
            }
            ++valueEnd;
        }

        if (auto* ops = target())
            (*ops)[name] = std::string (trim (std::string_view (source).substr (nameEnd + 1, valueEnd - nameEnd - 1)));
        i = valueEnd;
    }
    finishRegion();
    return regions;
}

} // namespace ck::library
