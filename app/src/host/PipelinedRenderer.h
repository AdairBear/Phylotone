// Runs a PluginHostSupervisor on a worker thread, so the caller never waits for a plugin (M4).
//
// submit() queues one block and returns at once. take() hands back the oldest finished
// block without waiting. Output therefore lags input by the queue depth. When the queue
// is full the new block is dropped and counted, so a slow or dead host never stalls the
// caller. Invariant: every submitted block is either taken or dropped, never both.
//
// Lock-free between the caller and the worker. Each direction is a single-producer,
// single-consumer ring of kMaxQueued slots: the caller submits and takes, the worker
// renders. The worker polls for work every kIdlePollMs when idle, so a block waits at most
// that long before it is rendered. Results that nobody takes are dropped (counted) when
// their ring is full, which keeps the worker from blocking on the caller.
#pragma once

#include "AudioBlockSink.h"
#include "PluginHostSupervisor.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class PipelinedRenderer : public AudioBlockSink
{
public:
    static constexpr std::size_t kMaxQueued = 4;

    explicit PipelinedRenderer(std::unique_ptr<PluginHostSupervisor> supervisor);
    ~PipelinedRenderer();

    PipelinedRenderer(const PipelinedRenderer&) = delete;
    PipelinedRenderer& operator=(const PipelinedRenderer&) = delete;

    // Caller side only (one thread). Queues a block. Returns false if the queue was full
    // and the block was dropped.
    bool submit(phylo::host::ProcessRequest block) override;

    // Caller side only (one thread). Swaps the oldest finished block into `out`. If none is
    // ready, `out` becomes `samples` zeros and the call returns false. Never waits.
    bool take(std::vector<float>& out, std::size_t samples) override;

    // Makes the worker kill the host as a crash would (test hook). Takes effect before
    // the next render on the worker.
    void requestKillForTesting();

    // Caller side only. Plugin state to restore on the worker before the next render. The
    // host keeps it and re-sends it after a restart. Not on the audio path.
    void restoreState(std::vector<std::uint8_t> bytes);
    // What the last restore did, for the status line. Empty until a restore has been asked for.
    std::string stateStatus() const;

    std::size_t droppedBlocks() const;
    std::size_t takenBlocks() const;

private:
    void work();

    std::unique_ptr<PluginHostSupervisor> supervisor_;

    // Rings. Counters only grow; a slot is index % kMaxQueued. A slot is written by its
    // producer before the counter that publishes it is released.
    std::array<phylo::host::ProcessRequest, kMaxQueued> requests_;
    std::array<std::vector<float>, kMaxQueued> results_;
    std::atomic<std::size_t> requestHead_{0}; // worker takes from here
    std::atomic<std::size_t> requestTail_{0}; // caller adds here
    std::atomic<std::size_t> resultHead_{0};  // caller takes from here
    std::atomic<std::size_t> resultTail_{0};  // worker adds here

    std::atomic<std::size_t> dropped_{0};
    std::atomic<std::size_t> taken_{0};
    std::atomic<bool> stop_{false};
    std::atomic<bool> killRequested_{false};
    std::atomic<std::vector<std::uint8_t>*> pendingState_{nullptr}; // owned by whoever holds it
    mutable std::mutex statusMutex_; // guards stateStatus_; never taken on the audio path
    std::string stateStatus_;
    std::thread worker_;
};
