#include "phylo/Scene.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace phylo
{

namespace
{

std::vector<std::string> splitWords(const std::string& s)
{
    std::vector<std::string> words;
    std::istringstream in(s);
    std::string w;
    while (in >> w)
        words.push_back(w);
    return words;
}

std::string stripComment(const std::string& line)
{
    const auto pos = line.find("//");
    return pos == std::string::npos ? line : line.substr(0, pos);
}

// Plain decimal numbers only: digits, sign, point and exponent. This rejects
// "nan", "inf" and hex, which strtod would otherwise accept.
bool parseDouble(const std::string& token, double& out)
{
    if (token.empty() || token.find_first_not_of("0123456789+-.eE") != std::string::npos)
        return false;
    char* end = nullptr;
    errno = 0;
    const double v = std::strtod(token.c_str(), &end);
    if (errno != 0 || end == token.c_str() || *end != '\0' || !std::isfinite(v))
        return false;
    out = v;
    return true;
}

bool parseInt(const std::string& token, int& out)
{
    double d = 0.0;
    if (!parseDouble(token, d) || std::fabs(d) > 1.0e9)
        return false;
    const int i = static_cast<int>(d);
    if (static_cast<double>(i) != d)
        return false;
    out = i;
    return true;
}

bool isNoteLetter(char c)
{
    return c >= 'A' && c <= 'G';
}

// A key root is a letter A to G with an optional # or b, and no octave.
bool isKeyRoot(const std::string& s)
{
    if (s.empty() || !isNoteLetter(s[0]))
        return false;
    if (s.size() == 1)
        return true;
    if (s.size() == 2 && (s[1] == '#' || s[1] == 'b'))
        return true;
    return false;
}

// Old Mac (CR-only) and Windows (CRLF) line endings become LF.
std::string normaliseLineEndings(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '\r')
        {
            out.push_back('\n');
            if (i + 1 < text.size() && text[i + 1] == '\n')
                ++i;
        }
        else
        {
            out.push_back(text[i]);
        }
    }
    return out;
}

} // namespace

const ScenePattern* Scene::findPattern(const std::string& name) const
{
    for (const auto& p : patterns)
        if (p.name == name)
            return &p;
    return nullptr;
}

const SceneMacro* Scene::findMacro(const std::string& name) const
{
    for (const auto& m : macros)
        if (m.name == name)
            return &m;
    return nullptr;
}

bool parseNoteToken(const std::string& token, int& midi)
{
    if (token.empty())
        return false;

    // A plain number is a MIDI note.
    if (token.find_first_not_of("0123456789") == std::string::npos)
    {
        int n = 0;
        if (!parseInt(token, n) || n < 0 || n > 127)
            return false;
        midi = n;
        return true;
    }

    if (!isNoteLetter(token[0]))
        return false;

    static const int kPitchClass[7] = {9, 11, 0, 2, 4, 5, 7}; // A B C D E F G
    int pc = kPitchClass[token[0] - 'A'];

    std::size_t i = 1;
    if (i < token.size() && (token[i] == '#' || token[i] == 'b'))
    {
        pc += (token[i] == '#') ? 1 : -1;
        ++i;
    }

    if (i >= token.size())
        return false;

    int octave = 0;
    if (!parseInt(token.substr(i), octave))
        return false;

    const int value = (octave + 1) * 12 + pc;
    if (value < 0 || value > 127)
        return false;
    midi = value;
    return true;
}

Pattern buildPattern(const ScenePattern& p)
{
    Pattern out(p.lengthBeats);
    for (const auto& n : p.notes)
        out.addNote(n.beat, static_cast<std::uint8_t>(n.note), static_cast<std::uint8_t>(n.velocity),
                    n.duration);
    return out;
}

bool samePattern(const ScenePattern& a, const ScenePattern& b)
{
    if (a.lengthBeats != b.lengthBeats || a.notes.size() != b.notes.size())
        return false;
    for (std::size_t i = 0; i < a.notes.size(); ++i)
    {
        const auto& x = a.notes[i];
        const auto& y = b.notes[i];
        if (x.beat != y.beat || x.note != y.note || x.velocity != y.velocity || x.duration != y.duration)
            return false;
    }
    return true;
}

ParseResult parseScene(const std::string& text)
{
    ParseResult result;
    Scene& scene = result.scene;

    auto error = [&result](int line, const std::string& msg) { result.errors.push_back({line, msg}); };

    enum class Block { None, Pattern, Section };
    Block block = Block::None;
    int activeLine = 0; // line of the top-level `play`, for the cross-check

    std::istringstream in(normaliseLineEndings(text));
    std::string raw;
    int lineNo = 0;

    while (std::getline(in, raw))
    {
        ++lineNo;
        const bool indented = !raw.empty() && (raw[0] == ' ' || raw[0] == '\t');
        const auto words = splitWords(stripComment(raw));
        if (words.empty())
            continue;

        if (indented)
        {
            if (block == Block::Pattern)
            {
                if (words.size() != 4)
                {
                    error(lineNo, "note line needs: beat note velocity duration");
                    continue;
                }
                SceneNote note;
                int midi = 0;
                if (!parseDouble(words[0], note.beat))
                    error(lineNo, "beat is not a number: " + words[0]);
                else if (!parseNoteToken(words[1], midi))
                    error(lineNo, "not a note: " + words[1]);
                else if (!parseInt(words[2], note.velocity) || note.velocity < 1 || note.velocity > 127)
                    error(lineNo, "velocity must be 1 to 127");
                else if (!parseDouble(words[3], note.duration) || note.duration <= 0.0)
                    error(lineNo, "duration must be a positive number of beats");
                else
                {
                    auto& p = scene.patterns.back();
                    if (note.beat < 0.0 || note.beat >= p.lengthBeats)
                        error(lineNo, "beat is outside the pattern length");
                    else
                    {
                        note.note = midi;
                        p.notes.push_back(note);
                    }
                }
            }
            else if (block == Block::Section)
            {
                if (words.size() == 2 && words[0] == "play")
                {
                    scene.sections.back().play.push_back(words[1]);
                    scene.sections.back().playLines.push_back(lineNo);
                }
                else
                    error(lineNo, "section lines must be: play <pattern>");
            }
            else
            {
                error(lineNo, "indented line is not inside a pattern or section");
            }
            continue;
        }

        // A top-level line closes the block above it.
        block = Block::None;
        const std::string& keyword = words[0];

        if (keyword == "tempo")
        {
            double bpm = 0.0;
            if (words.size() != 2 || !parseDouble(words[1], bpm) || bpm < 20.0 || bpm > 300.0)
                error(lineNo, "tempo needs a value from 20 to 300");
            else
                scene.tempo = bpm;
        }
        else if (keyword == "meter")
        {
            int beats = 0;
            if (words.size() != 2 || !parseInt(words[1], beats) || beats < 1 || beats > 16)
                error(lineNo, "meter needs a whole number from 1 to 16");
            else
                scene.meter = beats;
        }
        else if (keyword == "key")
        {
            if (words.size() != 3 || (words[2] != "major" && words[2] != "minor"))
            {
                error(lineNo, "key needs: key <root> major|minor");
            }
            else
            {
                if (!isKeyRoot(words[1]))
                    error(lineNo, "key root must be a note name without octave: " + words[1]);
                else
                {
                    scene.hasKey = true;
                    scene.keyRoot = words[1];
                    scene.keyMinor = words[2] == "minor";
                }
            }
        }
        else if (keyword == "play")
        {
            if (words.size() != 2)
                error(lineNo, "play needs a pattern name");
            else
            {
                scene.activePattern = words[1];
                activeLine = lineNo;
            }
        }
        else if (keyword == "macro")
        {
            double v = 0.0;
            if (words.size() != 3 || !parseDouble(words[2], v) || v < 0.0 || v > 1.0)
                error(lineNo, "macro needs: macro <name> <value from 0 to 1>");
            else if (scene.findMacro(words[1]) != nullptr)
                error(lineNo, "duplicate macro name: " + words[1]);
            else
                scene.macros.push_back({words[1], v, lineNo});
        }
        else if (keyword == "pattern")
        {
            double length = 0.0;
            if (words.size() != 3 || !parseDouble(words[2], length) || length <= 0.0 || length > 64.0)
            {
                error(lineNo, "pattern needs: pattern <name> <length in beats, above 0 and up to 64>");
                // Keep a placeholder block so its note lines do not cascade into more errors.
                block = Block::Pattern;
                scene.patterns.push_back({words.size() > 1 ? words[1] : "?", 4.0, lineNo, {}});
            }
            else if (scene.findPattern(words[1]) != nullptr)
            {
                error(lineNo, "duplicate pattern name: " + words[1]);
                block = Block::Pattern;
                scene.patterns.push_back({words[1] + "#dup", length, lineNo, {}});
            }
            else
            {
                block = Block::Pattern;
                scene.patterns.push_back({words[1], length, lineNo, {}});
            }
        }
        else if (keyword == "section")
        {
            int bars = 0;
            if (words.size() != 3 || !parseInt(words[2], bars) || bars < 1)
                error(lineNo, "section needs: section <name> <bars, at least 1>");
            else
            {
                block = Block::Section;
                scene.sections.push_back({words[1], bars, lineNo, {}, {}});
            }
        }
        else
        {
            error(lineNo, "unknown directive: " + keyword);
        }
    }

    // Cross-checks that need the whole file.
    if (!scene.activePattern.empty() && scene.findPattern(scene.activePattern) == nullptr)
        error(activeLine, "play refers to an unknown pattern: " + scene.activePattern);

    for (const auto& s : scene.sections)
        for (std::size_t i = 0; i < s.play.size(); ++i)
            if (scene.findPattern(s.play[i]) == nullptr)
                error(s.playLines[i], "section " + s.name + " plays an unknown pattern: " + s.play[i]);

    return result;
}

} // namespace phylo
