// Phylotone core: JUCE-free sequencing for M0.
//
// The sequencer is driven by an integer frame counter. Every event's absolute
// frame is computed from its beat position, and each block emits only the
// events whose frame falls in the half-open window [position, position + n).
// Because the window is half-open and integer-based, an event is emitted once
// and only once, whatever the block size.

#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace phylo
{

// A note-on or note-off inside a pattern. Beats are relative to the pattern start.
struct PatternEvent
{
    double beat = 0.0;
    std::uint8_t note = 60;
    std::uint8_t velocity = 100;
    bool on = true;
};

// A MIDI message to send, placed at a frame offset within the current block.
struct MidiOut
{
    int sampleOffset = 0;
    std::uint8_t status = 0;
    std::uint8_t data1 = 0;
    std::uint8_t data2 = 0;
};

// A looping pattern of notes, length in beats.
class Pattern
{
public:
    explicit Pattern(double lengthBeats = 4.0);

    double lengthBeats() const noexcept { return length; }

    // Adds a note. The note-off is clamped to the end of the pattern.
    void addNote(double beat, std::uint8_t note, std::uint8_t velocity, double durationBeats);

    // Events sorted by beat. At equal beats, note-offs come before note-ons, so a
    // retriggered note ends before it starts again.
    const std::vector<PatternEvent>& events() const noexcept { return events_; }

private:
    double length;
    std::vector<PatternEvent> events_; // kept sorted at all times
};

class Sequencer
{
public:
    Sequencer() = default;

    void setTempo(double bpm) noexcept { tempoBpm = bpm; }
    double tempo() const noexcept { return tempoBpm; }

    void setSampleRate(double sr) noexcept { sampleRate = sr; }
    double getSampleRate() const noexcept { return sampleRate; }

    void setPattern(Pattern p) { pattern = std::move(p); }
    const Pattern& getPattern() const noexcept { return pattern; }

    void play() noexcept { playing = true; }
    void stop() noexcept { playing = false; }
    bool isPlaying() const noexcept { return playing; }

    // Resets the playhead to the start of the pattern.
    void rewind() noexcept { position = 0; }

    // Current playhead in frames since play started, and in beats.
    std::int64_t positionFrames() const noexcept { return position; }
    double positionBeats() const noexcept;

    // Renders one block of `numFrames` frames. Appends MIDI messages to `out`
    // (the caller reserves capacity). Advances the playhead only while playing.
    void process(int numFrames, std::vector<MidiOut>& out);

    // Frames per beat at the current tempo and sample rate.
    double framesPerBeat() const noexcept;

private:
    Pattern pattern;
    double tempoBpm = 120.0;
    double sampleRate = 48000.0;
    bool playing = false;
    std::int64_t position = 0; // frames since rewind, not wrapped
};

} // namespace phylo
