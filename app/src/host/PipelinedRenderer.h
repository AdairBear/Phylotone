// Runs a PluginHostSupervisor on a worker thread, so the caller never waits for a plugin (M4).
//
// submit() queues one block and returns at once. take() hands back the oldest finished
// block without waiting. Output therefore lags input by the queue depth. When the queue
// is full the new block is dropped and counted, so a slow or dead host never stalls the
// caller. Invariant: every submitted block is either taken or dropped, never both.
#pragma once

#include "PluginHostSupervisor.h"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

class PipelinedRenderer
{
public:
    static constexpr std::size_t kMaxQueued = 4;

    explicit PipelinedRenderer(std::unique_ptr<PluginHostSupervisor> supervisor);
    ~PipelinedRenderer();

    PipelinedRenderer(const PipelinedRenderer&) = delete;
    PipelinedRenderer& operator=(const PipelinedRenderer&) = delete;

    // Queues a block. Returns false if the queue was full and the block was dropped.
    bool submit(phylo::host::ProcessRequest block);

    // Copies the oldest finished block into `out`. If none is ready, `out` becomes
    // `samples` zeros and the call returns false. Never waits.
    bool take(std::vector<float>& out, std::size_t samples);

    // Makes the worker kill the host as a crash would (test hook). Takes effect before
    // the next render on the worker.
    void requestKillForTesting();

    std::size_t droppedBlocks() const;
    std::size_t takenBlocks() const;

private:
    void work();

    std::unique_ptr<PluginHostSupervisor> supervisor_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<phylo::host::ProcessRequest> queued_;
    std::deque<std::vector<float>> done_;
    bool stop_ = false;
    bool killRequested_ = false;
    std::size_t dropped_ = 0;
    std::size_t taken_ = 0;
    std::thread worker_;
};
