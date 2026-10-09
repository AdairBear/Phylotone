// Renders a looping pattern block by block through a plugin, offline (M4).
//
// The renderer is a callback, so the same code runs against the plugin host in the
// app and against a test double in the headless check. Plain C++, no JUCE.
#pragma once

#include "phylo/Sequencer.h"
#include "phylo/host/Wire.h"

#include <cstddef>
#include <functional>
#include <vector>

// Fills `out` with the planar audio for one block. Must size `out` to frames * channels.
using BlockRenderer = std::function<void(const phylo::host::ProcessRequest& in, std::vector<float>& out)>;

struct OfflineTake
{
    std::vector<float> stereo; // interleaved L R, frames * 2 samples
    std::vector<std::size_t> noteOnFrames; // absolute frame of each note-on, in order
};

// Renders `frames` frames of `pattern` at `bpm`. The sequencer emits MIDI with a frame
// offset in each block; the offsets become absolute frames in noteOnFrames. The
// sequencer runs on a fixed sample rate, so changing blockFrames must not change the
// output or the note-on frames.
OfflineTake renderPatternOffline(const phylo::Pattern& pattern, double bpm, int sampleRate,
                                 std::size_t frames, const BlockRenderer& renderBlock,
                                 std::size_t blockFrames = 48);
