// Headless check of PipelinedRenderer (M4). Plain C++, no JUCE.
//
// Usage: phylo_renderer_check <path to phylo-plughost>
//
// Checks that:
//  - submit() never waits for the host (each call returns in a few ms),
//  - every block is accounted for: taken + dropped == submitted,
//  - a killed host does not stop submit() or take() from returning, and
//  - the output is silence of the requested size, with no plugin loaded.

#include "PipelinedRenderer.h"

#include <chrono>
#include <cstdio>
#include <thread>

namespace
{
int failures = 0;

#define CHECK(cond)                                                             \
    do                                                                          \
    {                                                                           \
        if (!(cond))                                                            \
        {                                                                       \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                 \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

phylo::host::ProcessRequest block()
{
    phylo::host::ProcessRequest r;
    r.frames = 64;
    r.channels = 2;
    r.audio.assign(128, 0.0f);
    return r;
}

using Clock = std::chrono::steady_clock;

long long msSince(Clock::time_point t0)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: phylo_renderer_check <phylo-plughost>\n");
        return 2;
    }

    PipelinedRenderer renderer(std::make_unique<PluginHostSupervisor>(argv[1], ""));
    std::vector<float> out;
    std::size_t submitted = 0;

    // 1. Submitting never waits. Tight loop, every submit must be quick.
    for (int i = 0; i < 40; ++i)
    {
        const auto t0 = Clock::now();
        renderer.submit(block());
        ++submitted;
        CHECK(msSince(t0) < 5);
    }

    // 2. Drain: take every result that comes back, within a bounded wait.
    std::size_t gotBlocks = 0;
    const auto drainStart = Clock::now();
    while (renderer.takenBlocks() + renderer.droppedBlocks() < submitted && msSince(drainStart) < 5000)
    {
        if (renderer.take(out, 128))
        {
            ++gotBlocks;
            CHECK(out.size() == 128);
            for (float f : out)
                CHECK(f == 0.0f);
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    CHECK(renderer.takenBlocks() + renderer.droppedBlocks() == submitted);
    CHECK(gotBlocks > 0);

    // 3. take() with nothing ready returns silence of the requested size, at once.
    {
        const auto t0 = Clock::now();
        CHECK(!renderer.take(out, 128));
        CHECK(out.size() == 128);
        CHECK(msSince(t0) < 5);
    }

    // 4. Kill the host. Submit and take must still return at once, and every block is still accounted for.
    renderer.requestKillForTesting();
    std::size_t before = renderer.takenBlocks() + renderer.droppedBlocks();
    for (int i = 0; i < 10; ++i)
    {
        const auto t0 = Clock::now();
        renderer.submit(block());
        ++submitted;
        renderer.take(out, 128);
        CHECK(msSince(t0) < 5);
    }
    const auto killStart = Clock::now();
    while (renderer.takenBlocks() + renderer.droppedBlocks() < submitted && msSince(killStart) < 5000)
    {
        renderer.take(out, 128);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(renderer.takenBlocks() + renderer.droppedBlocks() == submitted);
    CHECK(renderer.takenBlocks() + renderer.droppedBlocks() > before);

    if (failures == 0)
        std::printf("pipelined renderer check: ok (taken %zu, dropped %zu)\n",
                    renderer.takenBlocks(), renderer.droppedBlocks());
    return failures == 0 ? 0 : 1;
}
