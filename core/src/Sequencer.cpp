#include "phylo/Sequencer.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace phylo
{

namespace
{
bool usable(double v) noexcept
{
    return std::isfinite(v) && v > 0.0;
}
} // namespace

Pattern::Pattern(double lengthBeats) : length(lengthBeats > 0.0 ? lengthBeats : 1.0) {}

void Pattern::addNote(double beat, std::uint8_t note, std::uint8_t velocity, double durationBeats)
{
    if (beat < 0.0 || beat >= length)
        return; // outside the loop: ignored

    const double offBeat = std::min(beat + std::max(durationBeats, 0.0), length);

    PatternEvent on;
    on.beat = beat;
    on.note = note;
    on.velocity = velocity;
    on.on = true;

    PatternEvent off;
    off.beat = offBeat;
    off.note = note;
    off.velocity = 0;
    off.on = false;

    events_.push_back(on);
    events_.push_back(off);

    // Keep the list sorted. Note-offs sort before note-ons at equal beats.
    std::stable_sort(events_.begin(), events_.end(),
                     [](const PatternEvent& a, const PatternEvent& b)
                     {
                         if (a.beat != b.beat)
                             return a.beat < b.beat;
                         return !a.on && b.on;
                     });
}

void Sequencer::setTempo(double bpm) noexcept
{
    if (!usable(bpm))
        return;
    rebaseGrid();
    tempoBpm = bpm;
}

void Sequencer::setSampleRate(double sr) noexcept
{
    if (!usable(sr))
        return;
    rebaseGrid();
    sampleRate = sr;
}

void Sequencer::rebaseGrid() noexcept
{
    // Re-anchor the grid at the current playhead, using the old tempo, so the
    // beat under the playhead does not move when the tempo changes.
    gridBeat0 = beatAtFrame(static_cast<double>(position));
    gridFrame0 = position;
}

void Sequencer::setPattern(Pattern p)
{
    pattern = std::move(p);
    patternStartBeat = beatAtFrame(static_cast<double>(position));
    hasPending = false;
}

void Sequencer::scheduleNextBar(Pattern p)
{
    if (!usable(framesPerBeat()))
        return;

    // The next bar line strictly after the playhead, in beats.
    const double meter = static_cast<double>(meterBeats);
    const double here = beatAtFrame(static_cast<double>(position));
    auto k = static_cast<std::int64_t>(std::floor(here / meter)) + 1;
    double bar = static_cast<double>(k) * meter;
    while (static_cast<std::int64_t>(std::floor(frameOfBeat(bar))) <= position)
    {
        ++k;
        bar = static_cast<double>(k) * meter;
    }

    pending = std::move(p);
    switchBeat = bar;
    hasPending = true;
}

std::int64_t Sequencer::pendingSwitchFrame() const
{
    return hasPending ? switchFrame() : 0;
}

std::int64_t Sequencer::switchFrame() const noexcept
{
    return static_cast<std::int64_t>(std::floor(frameOfBeat(switchBeat)));
}

void Sequencer::rewind() noexcept
{
    position = 0;
    gridFrame0 = 0;
    gridBeat0 = 0.0;
    patternStartBeat = 0.0;
    hasPending = false;
}

double Sequencer::framesPerBeat() const noexcept
{
    return 60.0 * sampleRate / tempoBpm;
}

double Sequencer::framesPerBar() const noexcept
{
    return framesPerBeat() * meterBeats;
}

double Sequencer::beatAtFrame(double frame) const noexcept
{
    return gridBeat0 + (frame - static_cast<double>(gridFrame0)) / framesPerBeat();
}

double Sequencer::frameOfBeat(double beat) const noexcept
{
    return static_cast<double>(gridFrame0) + (beat - gridBeat0) * framesPerBeat();
}

double Sequencer::positionBeats() const noexcept
{
    return beatAtFrame(static_cast<double>(position));
}

void Sequencer::process(int numFrames, std::vector<MidiOut>& out)
{
    if (numFrames <= 0 || !playing)
        return; // silent, and the playhead stays where it is

    const std::int64_t blockStart = position;
    const std::int64_t blockEnd = position + numFrames;

    // Render in segments. A pending pattern takes over at its bar line, so the
    // block is split there and each part uses the pattern active for it.
    std::int64_t cursor = blockStart;
    while (cursor < blockEnd)
    {
        if (hasPending)
        {
            const std::int64_t swFrame = switchFrame();
            if (swFrame <= cursor)
            {
                releaseSounding(cursor, blockStart, out);
                pattern = std::move(pending);
                hasPending = false;
                patternStartBeat = switchBeat;
                continue;
            }
            if (swFrame < blockEnd)
            {
                emitRange(cursor, swFrame, blockStart, out);
                cursor = swFrame;
                continue;
            }
        }

        emitRange(cursor, blockEnd, blockStart, out);
        cursor = blockEnd;
    }

    position = blockEnd;
}

void Sequencer::releaseSounding(std::int64_t frame, std::int64_t blockStart, std::vector<MidiOut>& out)
{
    // The old pattern's note-offs at or after the bar line are not emitted, so
    // close whatever is still on here. Otherwise the note sticks.
    for (std::size_t note = 0; note < sounding.size(); ++note)
    {
        if (!sounding[note])
            continue;
        MidiOut m;
        m.sampleOffset = static_cast<int>(frame - blockStart);
        m.status = 0x80;
        m.data1 = static_cast<std::uint8_t>(note);
        m.data2 = 0;
        out.push_back(m);
        sounding.reset(note);
    }
}

void Sequencer::emitRange(std::int64_t from, std::int64_t to, std::int64_t blockStart,
                          std::vector<MidiOut>& out)
{
    if (from >= to || pattern.events().empty())
        return;

    const double len = pattern.lengthBeats();
    const double relFrom = beatAtFrame(static_cast<double>(from)) - patternStartBeat;
    const double relTo = beatAtFrame(static_cast<double>(to)) - patternStartBeat;

    // An event can round into the frame range of the loop before it, so check one
    // loop either side of the segment. The window filter keeps each event to the
    // segment it falls in. Each event's frame depends only on its beat and the
    // tempo, so the output does not depend on how the frames are split into blocks.
    const auto firstLoop = std::max<std::int64_t>(0, static_cast<std::int64_t>(std::floor(relFrom / len)) - 1);
    const auto lastLoop = std::max<std::int64_t>(firstLoop, static_cast<std::int64_t>(std::floor(relTo / len)) + 1);

    for (std::int64_t loop = firstLoop; loop <= lastLoop; ++loop)
    {
        const double loopStart = patternStartBeat + static_cast<double>(loop) * len;

        for (const auto& e : pattern.events())
        {
            const auto absFrame = static_cast<std::int64_t>(std::floor(frameOfBeat(loopStart + e.beat)));
            if (absFrame < from || absFrame >= to)
                continue;

            MidiOut m;
            m.sampleOffset = static_cast<int>(absFrame - blockStart);
            m.status = e.on ? 0x90 : 0x80; // channel 1 (index 0)
            m.data1 = e.note;
            m.data2 = e.velocity;
            out.push_back(m);

            if (e.note < sounding.size())
            {
                if (e.on)
                    sounding.set(e.note);
                else
                    sounding.reset(e.note);
            }
        }
    }
}

} // namespace phylo
