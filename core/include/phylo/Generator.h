// Seeded generator for pads and bass (M3). JUCE-free.
//
// A generated take is built from the scene's `chords` line (one chord per bar),
// its `seed`, its `generate` voices and its macros:
//
//   density  bass: how often extra notes are added (default 0.5)
//   tension  pads: adds a seventh to triads above 0.5 (default 0.3)
//   space    pads: how long each pad note sustains, and how loud (default 0.5)
//
// Determinism: the random source is a hand-written integer generator, not a
// standard library distribution, so the same seed and settings give the same
// notes on every compiler and platform. Every random draw is made whether or
// not it is used, so changing one macro does not reshuffle the other rolls.
#pragma once

#include "phylo/Scene.h"

namespace phylo
{

// The take the scene describes, as a pattern named "take". Its length is one
// bar per chord. Call only when scene.generate is not empty and scene.chords is
// not empty; otherwise returns an empty pattern of length 1 beat.
ScenePattern generateTake(const Scene& scene);

// True if the scene asks for a generated take.
bool hasGeneratedTake(const Scene& scene);

} // namespace phylo
