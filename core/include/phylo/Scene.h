// Scene language, first cut (M1). Line-based, JUCE-free.
//
// Example:
//
//   tempo 120           // BPM, 20 to 300
//   meter 4             // beats per bar, 1 to 16
//   key C minor         // parsed and stored; not used for generation yet
//   play bass           // the pattern the track plays
//   macro density 0.5   // a named control, 0 to 1
//   chords Cm Ab Eb Bb  // one chord per bar, 1 to 16 bars; the take loops over them
//   seed 7              // generator seed, 0 to 4294967295
//   generate pad bass   // generated voices; replaces the `play` pattern
//
//   pattern bass 4      // name, length in beats
//     0    C2 100 0.5   // beat, note (number or name), velocity, duration in beats
//     2    G2  90 0.5
//
//   section intro 8     // name, length in bars
//     play bass
//
// Comments start with //. Indented lines belong to the block above them.
// Sections are parsed and validated; arrangement playback is M6.
// Generated takes are described in phylo/Generator.h.

#pragma once

#include "phylo/Sequencer.h"

#include <cstdint>
#include <string>
#include <vector>

namespace phylo
{

struct SceneNote
{
    double beat = 0.0;
    int note = 60;
    int velocity = 100;
    double duration = 1.0;
};

struct ScenePattern
{
    std::string name;
    double lengthBeats = 4.0;
    int line = 0;
    std::vector<SceneNote> notes;
};

struct SceneSection
{
    std::string name;
    int bars = 4;
    int line = 0;
    std::vector<std::string> play;
    std::vector<int> playLines; // line of each `play`, same order as `play`
};

struct SceneError
{
    int line = 0; // 1-based
    std::string message;
};

enum class ChordQuality { Major, Minor, Dominant7, Minor7, Major7, Diminished };

struct SceneChord
{
    int rootPitchClass = 0; // 0 = C ... 11 = B
    ChordQuality quality = ChordQuality::Major;
    std::string symbol;     // as written, e.g. "Bb" or "F#m7"
    int line = 0;
};

struct SceneMacro
{
    std::string name;
    double value = 0.0; // 0 to 1
    int line = 0;
};

struct Scene
{
    double tempo = 120.0;
    int meter = 4;
    bool hasKey = false;
    std::string keyRoot;   // e.g. "C", "F#", "Bb"
    bool keyMinor = false;
    std::string activePattern; // from `play <name>` at top level
    std::vector<SceneMacro> macros;
    std::vector<ScenePattern> patterns;
    std::vector<SceneSection> sections;

    std::vector<SceneChord> chords;     // one per bar; empty if no `chords` line
    std::uint32_t seed = 1;             // from `seed`
    std::vector<std::string> generate;  // voices from `generate`; empty if none
    int generateLine = 0;

    const ScenePattern* findPattern(const std::string& name) const;
    const SceneMacro* findMacro(const std::string& name) const;
};

struct ParseResult
{
    Scene scene;
    std::vector<SceneError> errors;

    bool ok() const noexcept { return errors.empty(); }
};

// Parses scene text. Reports every error it finds, each with its line number.
// On error the scene may be partial, so callers should check ok() first.
ParseResult parseScene(const std::string& text);

// Builds a playable pattern from a parsed pattern.
Pattern buildPattern(const ScenePattern& p);

// True if two patterns have the same length and the same notes, in the same order.
bool samePattern(const ScenePattern& a, const ScenePattern& b);

// Parses a note name ("C4", "F#3", "Bb-1") or a MIDI number ("64") into 0..127.
// Returns false if the token is not a valid note.
bool parseNoteToken(const std::string& token, int& midi);

} // namespace phylo
