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

double Sequencer::framesPerBeat() const noexcept
{
    return 60.0 * sampleRate / tempoBpm;
}

double Sequencer::positionBeats() const noexcept
{
    return static_cast<double>(position) / framesPerBeat();
}

void Sequencer::process(int numFrames, std::vector<MidiOut>& out)
{
    if (numFrames <= 0)
        return;

    if (!playing || pattern.events().empty())
        return; // silent, and the playhead stays where it is

    const double fpb = framesPerBeat();
    const double patternFrames = pattern.lengthBeats() * fpb;
    const std::int64_t blockStart = position;
    const std::int64_t blockEnd = position + numFrames;

    // Pattern loops are indexed from zero. The loop length in frames is usually
    // not an integer, so an event can round down into the frame range of the loop
    // before it. Check one loop either side of the block so no event is missed;
    // the window filter below keeps each event to the block it falls in.
    const auto firstLoop = std::max<std::int64_t>(
        0, static_cast<std::int64_t>(std::floor(static_cast<double>(blockStart) / patternFrames)) - 1);
    const auto lastLoop = static_cast<std::int64_t>(std::floor(static_cast<double>(blockEnd) / patternFrames)) + 1;

    for (std::int64_t loop = firstLoop; loop <= lastLoop; ++loop)
    {
        const double loopOrigin = static_cast<double>(loop) * patternFrames;

        for (const auto& e : pattern.events())
        {
            // Absolute frame of this event. Rounding down keeps the first frame of an
            // event in the block it belongs to.
            const auto absFrame = static_cast<std::int64_t>(std::floor(loopOrigin + e.beat * fpb));

            if (absFrame < blockStart || absFrame >= blockEnd)
                continue;

            MidiOut m;
            m.sampleOffset = static_cast<int>(absFrame - blockStart);
            m.status = e.on ? 0x90 : 0x80; // channel 1 (index 0)
            m.data1 = e.note;
            m.data2 = e.velocity;
            out.push_back(m);
        }
    }

    position = blockEnd;
}

} // namespace phylo
