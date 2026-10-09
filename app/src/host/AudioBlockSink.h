// What the audio feed needs from a renderer: queue a block without waiting, and take the
// oldest finished block without waiting. PipelinedRenderer implements it; a test can use
// a synchronous fake. Plain C++, no JUCE.
#pragma once

#include "phylo/host/Wire.h"

#include <cstddef>
#include <vector>

class AudioBlockSink
{
public:
    virtual ~AudioBlockSink() = default;

    // Queues a block. Returns false if the block was dropped.
    virtual bool submit(phylo::host::ProcessRequest block) = 0;

    // Copies the oldest finished block into `out`, or `samples` zeros if none is ready.
    // Never waits.
    virtual bool take(std::vector<float>& out, std::size_t samples) = 0;
};
