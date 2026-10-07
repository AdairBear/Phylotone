#include "phylo/Sequencer.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace phylo
{

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

void Sequencer::setPattern(Pattern p)
{
    pattern = std::move(p);
    patternOrigin = position;
    hasPending = false;
}

void Sequencer::scheduleNextBar(Pattern p)
{
    const double bar = framesPerBar();
    auto k = static_cast<std::int64_t>(std::floor(static_cast<double>(position) / bar)) + 1;
    auto frame = static_cast<std::int64_t>(std::ceil(static_cast<double>(k) * bar));
    // Floating-point rounding can land the bar at or before the playhead. Move on a bar if so.
    while (frame <= position)
    {
        ++k;
        frame = static_cast<std::int64_t>(std::ceil(static_cast<double>(k) * bar));
    }

    pending = std::move(p);
    switchFrame = frame;
    hasPending = true;
}

void Sequencer::rewind() noexcept
{
    position = 0;
    patternOrigin = 0;
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

double Sequencer::positionBeats() const noexcept
{
    return static_cast<double>(position) / framesPerBeat();
}

void Sequencer::process(int numFrames, std::vector<MidiOut>& out)
{
    if (numFrames <= 0 || !playing)
        return; // silent, and the playhead stays where it is

    const std::int64_t blockStart = position;
    const std::int64_t blockEnd = position + numFrames;

    // Render in segments. A pending pattern takes over at its switch frame, so the
    // block is split there and each part uses the pattern that is active for it.
    std::int64_t cursor = blockStart;
    while (cursor < blockEnd)
    {
        if (hasPending && switchFrame <= cursor)
        {
            pattern = std::move(pending);
            hasPending = false;
            patternOrigin = switchFrame;
            continue;
        }

        std::int64_t segmentEnd = blockEnd;
        if (hasPending && switchFrame < segmentEnd)
            segmentEnd = switchFrame;

        emitRange(cursor, segmentEnd, blockStart, out);
        cursor = segmentEnd;
    }

    position = blockEnd;
}

void Sequencer::emitRange(std::int64_t from, std::int64_t to, std::int64_t blockStart,
                          std::vector<MidiOut>& out) const
{
    if (from >= to || pattern.events().empty())
        return;

    const double fpb = framesPerBeat();
    const double patternFrames = pattern.lengthBeats() * fpb;
    const double relFrom = static_cast<double>(from - patternOrigin);
    const double relTo = static_cast<double>(to - patternOrigin);

    // Loop length in frames is usually not an integer, so an event can round down
    // into the frame range of the loop before it. Check one loop either side of the
    // segment so no event is missed; the window filter keeps each event to one segment.
    const auto firstLoop = std::max<std::int64_t>(
        0, static_cast<std::int64_t>(std::floor(relFrom / patternFrames)) - 1);
    const auto lastLoop = static_cast<std::int64_t>(std::floor(relTo / patternFrames)) + 1;

    for (std::int64_t loop = firstLoop; loop <= lastLoop; ++loop)
    {
        const double loopOrigin = static_cast<double>(loop) * patternFrames;

        for (const auto& e : pattern.events())
        {
            // Absolute frame of this event. Rounding down keeps the first frame of an
            // event in the segment it belongs to.
            const auto absFrame =
                patternOrigin + static_cast<std::int64_t>(std::floor(loopOrigin + e.beat * fpb));

            if (absFrame < from || absFrame >= to)
                continue;

            MidiOut m;
            m.sampleOffset = static_cast<int>(absFrame - blockStart);
            m.status = e.on ? 0x90 : 0x80; // channel 1 (index 0)
            m.data1 = e.note;
            m.data2 = e.velocity;
            out.push_back(m);
        }
    }
}

} // namespace phylo
