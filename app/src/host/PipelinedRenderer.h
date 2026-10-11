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
    ~PipelinedRenderer() override;

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

    // Caller side only (the message thread, never the audio thread). An `akazi.samples.v1`
    // blob to send on the worker before the next render. Only the newest blob is kept: one
    // that the worker has not picked up yet is replaced.
    void sendSamples(std::vector<std::uint8_t> blob);
    // What the samples did, for the status line: "samples queued", "samples accepted",
    // "samples refused: <reason>" (the host's answer, or a set given up after the host failed
    // on it twice) or "samples not sent: <reason>" (no host answered). It follows the host:
    // when a restart sends the set again, the worker updates it with that outcome too, unless
    // a newer blob is queued. Empty until samples have been sent.
    std::string samplesStatus() const;
    // Counts sendSamples calls the worker has finished with (accepted or not), so a caller
    // can tell when the status belongs to the blob it sent.
    std::size_t samplesAnswered() const;

    std::size_t droppedBlocks() const;
    std::size_t takenBlocks() const;

private:
    void work();
    // Worker only. Refreshes samplesStatus_ when the supervisor has finished a samples exchange
    // since the last call (in setSamples or in a re-send after a restart).
    void publishSamplesOutcome();

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
    // A blob waiting for the worker, with the number of the sendSamples call that made it.
    struct PendingSamples
    {
        std::vector<std::uint8_t> blob;
        std::uint64_t seq = 0;
    };
    std::atomic<PendingSamples*> pendingSamples_{nullptr}; // owned by whoever holds it
    std::atomic<std::size_t> samplesAnswered_{0};
    mutable std::mutex statusMutex_; // guards stateStatus_ and samplesStatus_; never taken on the audio path
    std::string stateStatus_;
    std::string samplesStatus_;
    std::uint64_t samplesSeq_ = 0; // sendSamples calls so far; guarded by statusMutex_
    std::uint64_t handledSeq_ = 0;          // number of the blob the worker took last; worker only
    std::uint64_t publishedExchanges_ = 0;  // supervisor exchange count last published; worker only
    std::thread worker_;
};
