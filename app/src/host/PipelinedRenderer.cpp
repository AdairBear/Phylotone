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

void PipelinedRenderer::work()
{
    while (!stop_.load(std::memory_order_acquire))
    {
        if (killRequested_.exchange(false, std::memory_order_acq_rel))
            supervisor_->killHostForTesting();

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
