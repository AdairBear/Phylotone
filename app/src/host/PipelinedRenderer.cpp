#include "PipelinedRenderer.h"

PipelinedRenderer::PipelinedRenderer(std::unique_ptr<PluginHostSupervisor> supervisor)
    : supervisor_(std::move(supervisor)), worker_([this] { work(); })
{
}

PipelinedRenderer::~PipelinedRenderer()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    worker_.join();
}

bool PipelinedRenderer::submit(phylo::host::ProcessRequest block)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queued_.size() >= kMaxQueued)
        {
            ++dropped_;
            return false;
        }
        queued_.push_back(std::move(block));
    }
    wake_.notify_one();
    return true;
}

bool PipelinedRenderer::take(std::vector<float>& out, std::size_t samples)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (done_.empty())
    {
        out.assign(samples, 0.0f);
        return false;
    }
    out = std::move(done_.front());
    done_.pop_front();
    ++taken_;
    return true;
}

void PipelinedRenderer::requestKillForTesting()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        killRequested_ = true;
    }
    wake_.notify_one();
}

std::size_t PipelinedRenderer::droppedBlocks() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
}

std::size_t PipelinedRenderer::takenBlocks() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return taken_;
}

void PipelinedRenderer::work()
{
    for (;;)
    {
        phylo::host::ProcessRequest request;
        bool kill = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stop_ || killRequested_ || !queued_.empty(); });
            if (stop_)
                return;
            kill = killRequested_;
            killRequested_ = false;
            if (!queued_.empty())
            {
                request = std::move(queued_.front());
                queued_.pop_front();
            }
        }

        if (kill)
            supervisor_->killHostForTesting();

        if (request.frames == 0)
            continue;

        std::vector<float> result;
        supervisor_->render(request, result);

        std::lock_guard<std::mutex> lock(mutex_);
        done_.push_back(std::move(result));
        if (done_.size() > kMaxQueued)
        {
            // Nobody is taking. Keep the newest so the output stays current.
            done_.pop_front();
            ++dropped_;
        }
    }
}
