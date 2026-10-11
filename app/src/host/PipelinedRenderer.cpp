#include "PipelinedRenderer.h"

#include <chrono>
#include <memory>
#include <utility>

namespace
{
constexpr auto kIdlePollMs = std::chrono::milliseconds(1);
}

PipelinedRenderer::PipelinedRenderer(std::unique_ptr<PluginHostSupervisor> supervisor)
    : supervisor_(std::move(supervisor)), worker_([this] { work(); })
{
}

PipelinedRenderer::~PipelinedRenderer()
{
    stop_.store(true, std::memory_order_release);
    worker_.join();
    delete pendingState_.exchange(nullptr);
    delete pendingSamples_.exchange(nullptr);
}

void PipelinedRenderer::restoreState(std::vector<std::uint8_t> bytes)
{
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        stateStatus_ = "plugin state queued";
    }
    auto* incoming = new std::vector<std::uint8_t>(std::move(bytes));
    delete pendingState_.exchange(incoming, std::memory_order_acq_rel);
}

std::string PipelinedRenderer::stateStatus() const
{
    std::lock_guard<std::mutex> lock(statusMutex_);
    return stateStatus_;
}

void PipelinedRenderer::sendSamples(std::vector<std::uint8_t> blob)
{
    auto* incoming = new PendingSamples{std::move(blob), 0};
    {
        // The status is set before the blob is published, so the worker's answer to this
        // blob can never be overwritten by this "queued".
        std::lock_guard<std::mutex> lock(statusMutex_);
        incoming->seq = ++samplesSeq_;
        samplesStatus_ = "samples queued";
    }
    delete pendingSamples_.exchange(incoming, std::memory_order_acq_rel);
}

std::string PipelinedRenderer::samplesStatus() const
{
    std::lock_guard<std::mutex> lock(statusMutex_);
    return samplesStatus_;
}

std::size_t PipelinedRenderer::samplesAnswered() const
{
    return samplesAnswered_.load(std::memory_order_acquire);
}

bool PipelinedRenderer::submit(phylo::host::ProcessRequest block)
{
    const std::size_t tail = requestTail_.load(std::memory_order_relaxed);
    const std::size_t head = requestHead_.load(std::memory_order_acquire);
    if (tail - head >= kMaxQueued)
    {
        dropped_.fetch_add(1, std::memory_order_acq_rel);
        return false;
    }
    // The slot was moved out by the worker, so this move-assign frees nothing.
    requests_[tail % kMaxQueued] = std::move(block);
    requestTail_.store(tail + 1, std::memory_order_release);
    return true;
}

bool PipelinedRenderer::take(std::vector<float>& out, std::size_t samples)
{
    const std::size_t head = resultHead_.load(std::memory_order_relaxed);
    const std::size_t tail = resultTail_.load(std::memory_order_acquire);
    if (head == tail)
    {
        out.assign(samples, 0.0f);
        return false;
    }
    // Swap, not move-assign: the caller's old buffer goes to the slot and is freed on the
    // worker when the slot is next written, not here.
    std::swap(out, results_[head % kMaxQueued]);
    resultHead_.store(head + 1, std::memory_order_release);
    taken_.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

void PipelinedRenderer::requestKillForTesting()
{
    killRequested_.store(true, std::memory_order_release);
}

std::size_t PipelinedRenderer::droppedBlocks() const
{
    return dropped_.load(std::memory_order_acquire);
}

std::size_t PipelinedRenderer::takenBlocks() const
{
    return taken_.load(std::memory_order_acquire);
}

void PipelinedRenderer::publishSamplesOutcome()
{
    const std::uint64_t exchanges = supervisor_->samplesExchangeCount();
    if (exchanges == publishedExchanges_)
        return;
    publishedExchanges_ = exchanges;

    // A refusal is the host's answer (or a set given up after the host failed on it). Anything
    // else means the blob never got an answer.
    const auto outcome = supervisor_->samplesOutcome();
    std::string status = "samples accepted";
    if (!outcome.accepted)
        status = std::string(outcome.refused ? "samples refused: " : "samples not sent: ") + outcome.error;

    // Only the newest blob writes the status. If another sendSamples came in after the one
    // this worker last took, its "queued" stays until that blob is answered.
    std::lock_guard<std::mutex> lock(statusMutex_);
    if (handledSeq_ == samplesSeq_)
        samplesStatus_ = status;
}

void PipelinedRenderer::work()
{
    while (!stop_.load(std::memory_order_acquire))
    {
        if (killRequested_.exchange(false, std::memory_order_acq_rel))
            supervisor_->killHostForTesting();

        if (std::vector<std::uint8_t>* bytes = pendingState_.exchange(nullptr, std::memory_order_acq_rel))
        {
            std::unique_ptr<std::vector<std::uint8_t>> owned(bytes);
            const bool ok = supervisor_->setState(*owned);
            std::lock_guard<std::mutex> lock(statusMutex_);
            stateStatus_ = ok ? "plugin state restored" : "plugin state not accepted: " + supervisor_->lastError();
        }
        publishSamplesOutcome(); // a start inside setState re-sends the samples

        if (PendingSamples* pending = pendingSamples_.exchange(nullptr, std::memory_order_acq_rel))
        {
            std::unique_ptr<PendingSamples> owned(pending);
            handledSeq_ = owned->seq;
            supervisor_->setSamples(owned->blob);
            publishSamplesOutcome();
            samplesAnswered_.fetch_add(1, std::memory_order_acq_rel);
        }

        const std::size_t head = requestHead_.load(std::memory_order_relaxed);
        const std::size_t tail = requestTail_.load(std::memory_order_acquire);
        if (head == tail)
        {
            std::this_thread::sleep_for(kIdlePollMs);
            continue;
        }

        phylo::host::ProcessRequest request = std::move(requests_[head % kMaxQueued]);
        requestHead_.store(head + 1, std::memory_order_release);

        if (request.frames == 0)
            continue;

        std::vector<float> result;
        supervisor_->render(request, result);
        publishSamplesOutcome(); // a restart inside render re-sends the samples

        const std::size_t rHead = resultHead_.load(std::memory_order_acquire);
        const std::size_t rTail = resultTail_.load(std::memory_order_relaxed);
        if (rTail - rHead >= kMaxQueued)
        {
            // Nobody is taking. The block is dropped, so it is counted as dropped.
            dropped_.fetch_add(1, std::memory_order_acq_rel);
            continue;
        }
        results_[rTail % kMaxQueued] = std::move(result);
        resultTail_.store(rTail + 1, std::memory_order_release);
    }
}
