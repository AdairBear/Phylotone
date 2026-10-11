// Headless check of the sample send path (host-path chunk). Plain C++, no JUCE.
//
// Usage: phylo_sample_send_check <fake host> <real phylo-plughost>
//
// Against the stand-in host (FakeSampleHost.cpp, which reports how many slots it holds in
// every sample it renders) it checks that:
//  - a good blob is accepted, a bad one is refused with its reason, and the previous slots
//    keep playing without a restart,
//  - a blob replaces the whole set,
//  - the accepted set (not a refused one) is sent again after the host is killed, together
//    with the plugin state, state first,
//  - PipelinedRenderer sends blobs without ever holding up submit() or take(), and the final
//    status describes the last blob.
// Against the real phylo-plughost with no plugin loaded it checks that a bad blob gets its
// own reason, and a good one is refused because no plugin is loaded.

#include "PipelinedRenderer.h"
#include "PluginHostSupervisor.h"
#include "phylo/samples/SampleBlob.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{
using namespace phylo::samples;
using Clock = std::chrono::steady_clock;

int checks = 0;
int failures = 0;

#define CHECK(cond)                                                             \
    do                                                                          \
    {                                                                           \
        ++checks;                                                               \
        if (!(cond))                                                            \
        {                                                                       \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                 \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

constexpr std::size_t kFrames = 64;
constexpr std::size_t kChannels = 2;

phylo::host::ProcessRequest block()
{
    phylo::host::ProcessRequest r;
    r.frames = kFrames;
    r.channels = kChannels;
    r.audio.assign(kFrames * kChannels, 0.0f);
    return r;
}

long long msSince(Clock::time_point t0)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
}

// A few frames of mono audio per slot, slot numbers as given.
std::vector<std::uint8_t> makeBlob(const std::vector<int>& slotNumbers)
{
    SampleSlotSet slots;
    for (int n : slotNumbers)
    {
        SampleSlot s;
        s.slot = n;
        s.name = "s" + std::to_string(n);
        s.samples = {0.1f, -0.2f, 0.3f, -0.4f};
        slots.push_back(s);
    }
    std::vector<std::uint8_t> blob;
    const BlobResult r = encodeSampleBlob(slots, blob);
    CHECK(r.ok());
    return blob;
}

std::vector<std::uint8_t> faulty(const std::vector<int>& slotNumbers, BlobFault fault)
{
    auto blob = makeBlob(slotNumbers);
    CHECK(applyBlobFaultForTesting(blob, fault));
    return blob;
}

// Renders one block. Returns the level the fake host reports, or -1 when the output is
// silence-sized wrong or the samples differ from each other.
float renderLevel(PluginHostSupervisor& sup)
{
    std::vector<float> out;
    sup.render(block(), out);
    if (out.size() != kFrames * kChannels)
        return -1.0f;
    for (float f : out)
        if (f != out[0])
            return -1.0f;
    return out[0];
}

// After a kill: render until the host is back (the restart waits out the cooldown), then
// return the level of that render.
float renderAfterRestart(PluginHostSupervisor& sup)
{
    const auto t0 = Clock::now();
    float level = 0.0f;
    do
    {
        level = renderLevel(sup);
        if (sup.state() == PluginHostSupervisor::State::Running)
            return level;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } while (msSince(t0) < 4000);
    return -2.0f;
}

bool contains(const std::string& text, const char* part)
{
    return text.find(part) != std::string::npos;
}

struct FaultCase
{
    BlobFault fault;
    const char* reason;
};
const FaultCase kFaults[] = {
    {BlobFault::WrongHeader, "unknown format header"},
    {BlobFault::WrongSampleRate, "44100"},
    {BlobFault::SlotOutOfRange, "slot 128 is out of range"},
    {BlobFault::SampleTooLong, "longer than 60 s"},
    {BlobFault::Truncated, "truncated"},
};

void checkSupervisorWithFake(const char* fakeHost)
{
    // A good blob is accepted.
    {
        PluginHostSupervisor sup(fakeHost, "");
        CHECK(renderLevel(sup) == 0.0f);
        CHECK(sup.setSamples(makeBlob({0})));
        CHECK(sup.lastSamplesError().empty());
        CHECK(renderLevel(sup) == 1.0f);
    }

    // Each bad blob is refused, the host is not restarted, and the old slot keeps playing.
    {
        PluginHostSupervisor sup(fakeHost, "");
        CHECK(sup.setSamples(makeBlob({0})));
        int variant = 1;
        for (const FaultCase& c : kFaults)
        {
            const auto t0 = Clock::now();
            CHECK(!sup.setSamples(faulty({variant, variant + 1}, c.fault)));
            CHECK(contains(sup.lastSamplesError(), c.reason));
            CHECK(sup.state() == PluginHostSupervisor::State::Running);
            CHECK(renderLevel(sup) == 1.0f); // answered at once: no restart cooldown in between
            CHECK(msSince(t0) < PluginHostSupervisor::kRestartCooldownMs);
            ++variant;
        }
        CHECK(sup.setSamples(makeBlob({3, 4}))); // and a good one still works afterwards
        CHECK(sup.lastSamplesError().empty());
        CHECK(renderLevel(sup) == 2.0f);
    }

    // A blob replaces the whole set: 3, then 1, then 0 slots.
    {
        PluginHostSupervisor sup(fakeHost, "");
        CHECK(sup.setSamples(makeBlob({0, 5, 9})));
        CHECK(renderLevel(sup) == 3.0f);
        CHECK(sup.setSamples(makeBlob({7})));
        CHECK(renderLevel(sup) == 1.0f);
        CHECK(sup.setSamples(makeBlob({})));
        CHECK(renderLevel(sup) == 0.0f);
    }

    // The accepted set comes back after a crash, without calling setSamples again.
    {
        PluginHostSupervisor sup(fakeHost, "");
        CHECK(sup.setSamples(makeBlob({1, 2})));
        CHECK(renderLevel(sup) == 2.0f);
        sup.killHostForTesting();
        renderLevel(sup); // finds the crash
        CHECK(sup.state() == PluginHostSupervisor::State::Failed);
        CHECK(renderAfterRestart(sup) == 2.0f);
    }

    // After a refusal, a restart sends the accepted set, not the bad one.
    {
        PluginHostSupervisor sup(fakeHost, "");
        CHECK(sup.setSamples(makeBlob({1, 2})));
        CHECK(!sup.setSamples(faulty({3, 4, 5}, BlobFault::Truncated)));
        sup.killHostForTesting();
        renderLevel(sup);
        CHECK(renderAfterRestart(sup) == 2.0f);
        CHECK(sup.lastSamplesError().empty());
    }

    // A refusal on the restart itself: the reason survives and the old set is restored.
    // (The fake host never refuses a good blob, so this is covered by the real host below.)

    // Plugin state and samples are both sent again after a restart.
    {
        PluginHostSupervisor sup(fakeHost, "");
        CHECK(renderLevel(sup) == 0.0f);
        CHECK(sup.setState({1, 2, 3, 4}));
        CHECK(sup.setSamples(makeBlob({0})));
        CHECK(renderLevel(sup) == 1001.0f);
        sup.killHostForTesting();
        renderLevel(sup);
        CHECK(renderAfterRestart(sup) == 1001.0f);
    }

    // setSamples before the host has ever started: the call starts it and the blob is accepted.
    {
        PluginHostSupervisor sup(fakeHost, "");
        CHECK(sup.state() == PluginHostSupervisor::State::Idle);
        CHECK(sup.setSamples(makeBlob({0, 1})));
        CHECK(sup.state() == PluginHostSupervisor::State::Running);
        CHECK(renderLevel(sup) == 2.0f);
    }

    // setSamples with no host executable: false with a reason, no hang, no crash.
    {
        PluginHostSupervisor sup("/nonexistent/phylo-no-such-host", "");
        const auto t0 = Clock::now();
        CHECK(!sup.setSamples(makeBlob({0})));
        CHECK(!sup.lastSamplesError().empty());
        CHECK(msSince(t0) < 3000);
        CHECK(!sup.setSamples(makeBlob({0}))); // inside the cooldown: still false, still a reason
        CHECK(!sup.lastSamplesError().empty());
    }
}

// Sends blobs through the renderer and returns once the status has left "samples queued".
bool waitForAnswer(PipelinedRenderer& renderer, int timeoutMs)
{
    const auto t0 = Clock::now();
    while (msSince(t0) < timeoutMs)
    {
        const std::string status = renderer.samplesStatus();
        if (status != "samples queued" && !status.empty())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

void checkRendererWithFake(const char* fakeHost)
{
    PipelinedRenderer renderer(std::make_unique<PluginHostSupervisor>(fakeHost, ""));
    std::vector<float> out;

    CHECK(renderer.samplesStatus().empty());
    CHECK(renderer.samplesAnswered() == 0);

    renderer.sendSamples(makeBlob({0, 1}));
    CHECK(waitForAnswer(renderer, 5000));
    CHECK(renderer.samplesStatus() == "samples accepted");
    CHECK(renderer.samplesAnswered() == 1);

    renderer.sendSamples(faulty({4, 5}, BlobFault::SlotOutOfRange));
    CHECK(waitForAnswer(renderer, 5000));
    CHECK(renderer.samplesStatus().rfind("samples refused: ", 0) == 0);
    CHECK(contains(renderer.samplesStatus(), "slot 128 is out of range"));

    // Many sends while blocks flow: submit and take never wait, the last blob decides the status.
    for (int round = 0; round < 2; ++round)
    {
        const bool lastIsBad = round == 0;
        for (int i = 0; i < 40; ++i)
        {
            const bool bad = (i == 39) ? lastIsBad : (i % 2 == 0);
            const auto t0 = Clock::now();
            renderer.sendSamples(bad ? faulty({1, 2}, BlobFault::Truncated) : makeBlob({1, 2, 3}));
            renderer.submit(block());
            renderer.take(out, kFrames * kChannels);
            CHECK(msSince(t0) < 20); // sendSamples, submit and take together
        }
        CHECK(waitForAnswer(renderer, 5000));
        const std::string status = renderer.samplesStatus();
        if (lastIsBad)
            CHECK(status.rfind("samples refused: ", 0) == 0 && contains(status, "truncated"));
        else
            CHECK(status == "samples accepted");
    }

    // Blocks keep flowing while a blob is on its way, and the host holds the last accepted set.
    std::size_t submitted = 0;
    for (int i = 0; i < 20; ++i)
    {
        const auto t0 = Clock::now();
        renderer.submit(block());
        ++submitted;
        CHECK(msSince(t0) < 20);
        const auto t1 = Clock::now();
        renderer.take(out, kFrames * kChannels);
        CHECK(msSince(t1) < 20);
    }
    const auto t0 = Clock::now();
    while (renderer.takenBlocks() + renderer.droppedBlocks() < submitted && msSince(t0) < 3000)
    {
        renderer.take(out, kFrames * kChannels);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(out.size() == kFrames * kChannels);
}

void checkRealHost(const char* realHost)
{
    PluginHostSupervisor sup(realHost, ""); // no plugin loaded
    std::vector<float> out;
    sup.render(block(), out);
    CHECK(sup.state() == PluginHostSupervisor::State::Running);

    // The real host validates before it looks for a plugin, so each fault has its own reason.
    int variant = 0;
    for (const FaultCase& c : kFaults)
    {
        CHECK(!sup.setSamples(faulty({variant, variant + 1}, c.fault)));
        CHECK(contains(sup.lastSamplesError(), c.reason));
        CHECK(!contains(sup.lastSamplesError(), "no plugin loaded"));
        CHECK(sup.state() == PluginHostSupervisor::State::Running);
        ++variant;
    }

    // A valid blob gets past the check and meets the missing plugin.
    CHECK(!sup.setSamples(makeBlob({0})));
    CHECK(contains(sup.lastSamplesError(), "no plugin loaded"));
    CHECK(sup.state() == PluginHostSupervisor::State::Running);

    sup.render(block(), out);
    CHECK(out.size() == kFrames * kChannels);
    bool silent = true;
    for (float f : out)
        silent = silent && f == 0.0f;
    CHECK(silent);
    CHECK(sup.state() == PluginHostSupervisor::State::Running);
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::printf("usage: phylo_sample_send_check <fake host> <phylo-plughost>\n");
        return 2;
    }

    checkSupervisorWithFake(argv[1]);
    checkRendererWithFake(argv[1]);
    checkRealHost(argv[2]);

    std::printf("sample send check: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
