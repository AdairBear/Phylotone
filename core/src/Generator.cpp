#include "phylo/Generator.h"

#include <algorithm>
#include <cstdint>

namespace phylo
{

namespace
{

// SplitMix32-style mixer, integer only, so results do not depend on the platform.
class Rng
{
public:
    explicit Rng(std::uint32_t seed) : state(seed ^ 0x9E3779B9u) {}

    std::uint32_t next()
    {
        state += 0x9E3779B9u;
        std::uint32_t z = state;
        z = (z ^ (z >> 16)) * 0x85EBCA6Bu;
        z = (z ^ (z >> 13)) * 0xC2B2AE35u;
        return z ^ (z >> 16);
    }

    // A value in [0, 1). Integer division by a power of two is exact.
    double unit()
    {
        return static_cast<double>(next()) / 4294967296.0;
    }

private:
    std::uint32_t state;
};

double macroValue(const Scene& scene, const char* name, double fallback)
{
    const SceneMacro* m = scene.findMacro(name);
    return m != nullptr ? m->value : fallback;
}

bool hasVoice(const Scene& scene, const char* voice)
{
    return std::find(scene.generate.begin(), scene.generate.end(), voice) != scene.generate.end();
}

int clampMidi(int n)
{
    return std::max(0, std::min(127, n));
}

// Chord tones as semitones above the root.
struct Tones
{
    int third;
    int fifth;
    int seventh; // -1 if the chord has none
};

Tones tonesFor(ChordQuality q)
{
    switch (q)
    {
    case ChordQuality::Minor:      return {3, 7, -1};
    case ChordQuality::Diminished: return {3, 6, -1};
    case ChordQuality::Dominant7:  return {4, 7, 10};
    case ChordQuality::Minor7:     return {3, 7, 10};
    case ChordQuality::Major7:     return {4, 7, 11};
    case ChordQuality::Major:
    default:                       return {4, 7, -1};
    }
}

} // namespace

bool hasGeneratedTake(const Scene& scene)
{
    return !scene.generate.empty() && !scene.chords.empty();
}

ScenePattern generateTake(const Scene& scene)
{
    ScenePattern take;
    take.name = "take";
    take.line = scene.generateLine;

    if (!hasGeneratedTake(scene))
    {
        take.lengthBeats = 1.0;
        return take;
    }

    const double meter = static_cast<double>(scene.meter);
    const int bars = static_cast<int>(scene.chords.size());
    take.lengthBeats = meter * bars;

    const double density = macroValue(scene, "density", 0.5);
    const double tension = macroValue(scene, "tension", 0.3);
    const double space = macroValue(scene, "space", 0.5);

    const bool pads = hasVoice(scene, "pad");
    const bool bass = hasVoice(scene, "bass");

    Rng rng(scene.seed);

    for (int bar = 0; bar < bars; ++bar)
    {
        const SceneChord& chord = scene.chords[static_cast<std::size_t>(bar)];
        const double barStart = meter * bar;
        const Tones tones = tonesFor(chord.quality);

        if (pads)
        {
            // Voiced in the octave above C3 (MIDI 48). Notes above 72 move down an octave.
            const int root = 48 + chord.rootPitchClass;
            const bool useSeventh = tones.seventh >= 0 || tension > 0.5;
            // A tension seventh is a flat seventh: the colour of a dominant chord, added to a triad.
            const int seventh = tones.seventh >= 0 ? tones.seventh : 10;
            const int offsets[4] = {0, tones.third, tones.fifth, seventh};
            const int count = useSeventh ? 4 : 3;
            const int velocity = 56 + static_cast<int>(24.0 * space + 0.5);
            const double duration = meter * (0.5 + 0.5 * space);

            for (int i = 0; i < count; ++i)
            {
                int note = root + offsets[i];
                if (note > 72)
                    note -= 12;
                SceneNote n;
                n.beat = barStart;
                n.note = clampMidi(note);
                n.velocity = std::max(1, std::min(127, velocity));
                n.duration = duration;
                take.notes.push_back(n);
            }
        }

        if (bass)
        {
            // Bass sits in the octave of C2 (MIDI 36) and always plays the root on the downbeat.
            const int root = clampMidi(36 + chord.rootPitchClass);

            // Every candidate draws from the rng, used or not, so the rolls stay
            // in the same places when density changes.
            for (int beat = 0; beat < scene.meter; ++beat)
            {
                const double roll = rng.unit();
                const double accent = rng.unit();
                if (beat == 0)
                {
                    SceneNote n;
                    n.beat = barStart;
                    n.note = root;
                    n.velocity = 96 + static_cast<int>(accent * 8.0);
                    n.duration = std::min(meter, 1.0) * 0.8;
                    take.notes.push_back(n);
                    continue;
                }

                if (roll < density * 0.6)
                {
                    SceneNote n;
                    n.beat = barStart + beat;
                    n.note = root;
                    n.velocity = 80 + static_cast<int>(accent * 16.0);
                    n.duration = 0.4;
                    take.notes.push_back(n);
                }
            }

            // Off-beat eighths, drawn the same way.
            for (int beat = 0; beat < scene.meter; ++beat)
            {
                const double roll = rng.unit();
                const double accent = rng.unit();
                if (roll < density * 0.3)
                {
                    SceneNote n;
                    n.beat = barStart + beat + 0.5;
                    n.note = root;
                    n.velocity = 70 + static_cast<int>(accent * 16.0);
                    n.duration = 0.25;
                    take.notes.push_back(n);
                }
            }
        }
    }

    // Keep the note list in beat order, so equal inputs give equal lists.
    std::stable_sort(take.notes.begin(), take.notes.end(),
                     [](const SceneNote& a, const SceneNote& b) { return a.beat < b.beat; });

    return take;
}

} // namespace phylo
