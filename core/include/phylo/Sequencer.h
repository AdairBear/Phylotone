// Phylotone core: JUCE-free sequencing for M0 and M1.
//
// The sequencer is driven by an integer frame counter. Every event's absolute
// frame is computed from its beat position, and each block emits only the
// events whose frame falls in the half-open window [position, position + n).
// Because the window is half-open and integer-based, an event is emitted once
// and only once, whatever the block size.
//
// A pattern change can be scheduled with scheduleNextBar(). It takes effect at
// the next bar line, splitting the block there, so the new pattern starts
// exactly on the bar with no gap or repeated events.

#pragma once

#include <bitset>
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

    // Changing tempo or sample rate while playing keeps the playhead on the same
    // beat. Non-finite or non-positive values are ignored.
    void setTempo(double bpm) noexcept;
    double tempo() const noexcept { return tempoBpm; }

    void setSampleRate(double sr) noexcept;
    double getSampleRate() const noexcept { return sampleRate; }

    // Beats per bar. Bar lines fall every `beats` beats. Minimum 1.
    void setMeter(int beatsPerBar) noexcept { meterBeats = beatsPerBar > 0 ? beatsPerBar : 1; }
    int meter() const noexcept { return meterBeats; }

    // Replaces the pattern immediately. Its beat 0 lands on the current playhead.
    void setPattern(Pattern p);
    const Pattern& getPattern() const noexcept { return pattern; }

    // Schedules a pattern to replace the current one at the next bar line.
    // A second call before the bar replaces the first. Notes the old pattern
    // left sounding are released at the bar line.
    void scheduleNextBar(Pattern p);
    bool hasPendingPattern() const noexcept { return hasPending; }
    std::int64_t pendingSwitchFrame() const;

    void play() noexcept { playing = true; }
    void stop() noexcept { playing = false; }
    bool isPlaying() const noexcept { return playing; }

    // Resets the playhead to the start of the pattern. Drops any scheduled change.
    void rewind() noexcept;

    // Current playhead in frames since play started, and in beats.
    std::int64_t positionFrames() const noexcept { return position; }
    double positionBeats() const noexcept;

    // Renders one block of `numFrames` frames. Appends MIDI messages to `out`
    // (the caller reserves capacity). Advances the playhead only while playing.
    void process(int numFrames, std::vector<MidiOut>& out);

    // Frames per beat at the current tempo and sample rate.
    double framesPerBeat() const noexcept;

    // Frames per bar at the current tempo, sample rate and meter.
    double framesPerBar() const noexcept;

private:
    // Time is kept in beats. The bar grid and the pattern start are beats, and a
    // beat maps to a frame through the current tempo. That lets a tempo change
    // keep the playhead and the bar lines where they are.
    double beatAtFrame(double frame) const noexcept;
    double frameOfBeat(double beat) const noexcept;
    std::int64_t switchFrame() const noexcept;
    void emitRange(std::int64_t from, std::int64_t to, std::int64_t blockStart,
                   std::vector<MidiOut>& out);
    void releaseSounding(std::int64_t frame, std::int64_t blockStart, std::vector<MidiOut>& out);
    void rebaseGrid() noexcept;

    Pattern pattern;
    Pattern pending;
    bool hasPending = false;
    double switchBeat = 0.0;       // bar line at which `pending` takes over
    double patternStartBeat = 0.0; // beat of beat 0 of the current pattern
    double gridBeat0 = 0.0;        // beat at gridFrame0
    std::int64_t gridFrame0 = 0;   // frame that anchors the beat grid
    double tempoBpm = 120.0;
    double sampleRate = 48000.0;
    int meterBeats = 4;
    bool playing = false;
    std::int64_t position = 0; // frames since rewind, not wrapped
    std::bitset<128> sounding;  // notes currently on, for releasing at a switch
};

} // namespace phylo
