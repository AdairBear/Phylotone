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
//  - a blob that kills or stalls the host is tried once more and then given up, and the
//    accepted set plays again,
//  - a host that stops reading cannot hold a send, or the renderer's destructor, for ever,
//  - a stray extra answer is not taken for the answer to the next request, a long refusal
//    text is cut, non-blob bytes are refused without sending, and a refused plugin state
//    keeps its reason,
//  - PipelinedRenderer sends blobs without ever holding up submit() or take(), and its status
//    follows what the host did, also when a restart sends the set again.
// Against the real phylo-plughost with no plugin loaded it checks that a bad blob gets its
// own reason, and a good one is refused because no plugin is loaded.
//
// The fake host misbehaves on request through environment variables (see FakeSampleHost.cpp).
// The check sets them in its own process before it starts a supervisor; children inherit them.

#include "PipelinedRenderer.h"
#include "PluginHostSupervisor.h"
#include "phylo/samples/SampleBlob.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

// A call that should be prompt. Generous, because a loaded CI runner can be slow.
constexpr long long kPromptMs = 250;

// Sets an environment variable for the children this process starts, and clears it again.
struct EnvGuard
{
    EnvGuard(const char* nameIn, const char* value) : name(nameIn)
    {
#if defined(_WIN32)
        _putenv_s(name, value);
#else
        setenv(name, value, 1);
#endif
    }
    ~EnvGuard()
    {
#if defined(_WIN32)
        _putenv_s(name, ""); // an empty value removes the variable
#else
        unsetenv(name);
#endif
    }
    EnvGuard(const EnvGuard&) = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;
    const char* name;
};

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

void sleepMs(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// A few frames of mono audio per slot, slot numbers as given.
std::vector<std::uint8_t> makeBlob(const std::vector<int>& slotNumbers, std::size_t framesPerSlot = 4)
{
    SampleSlotSet slots;
    for (int n : slotNumbers)
    {
        SampleSlot s;
        s.slot = n;
        s.name = "s" + std::to_string(n);
        s.samples.assign(framesPerSlot, 0.1f);
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

// Four slots of about 1.2 MB each: a blob of nearly 5 MB.
std::vector<std::uint8_t> bigBlob()
{
    auto blob = makeBlob({10, 11, 12, 13}, 300000);
    CHECK(blob.size() > 4000000);
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
float renderAfterRestart(PluginHostSupervisor& sup, int limitMs = 4000)
{
    const auto t0 = Clock::now();
    float level = 0.0f;
    do
    {
        level = renderLevel(sup);
        if (sup.state() == PluginHostSupervisor::State::Running)
            return level;
        sleepMs(20);
    } while (msSince(t0) < limitMs);
    return -2.0f;
}

// Kills the host and renders once, which finds the crash and starts the cooldown.
void crashHost(PluginHostSupervisor& sup)
{
    sup.killHostForTesting();
    renderLevel(sup);
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

    // After a refusal, a restart sends the accepted set, not the bad one. The reason of the
    // refusal stays: it is still the answer to the last setSamples.
    {
        PluginHostSupervisor sup(fakeHost, "");
        CHECK(sup.setSamples(makeBlob({1, 2})));
        CHECK(!sup.setSamples(faulty({3, 4, 5}, BlobFault::Truncated)));
        sup.killHostForTesting();
        renderLevel(sup);
        CHECK(renderAfterRestart(sup) == 2.0f);
        CHECK(contains(sup.lastSamplesError(), "truncated"));
    }

    // A refusal on the restart itself: the set is accepted, the host dies, a faulty blob is
    // asked for while it is down (so it cannot be sent), and the restart refuses it. The old
    // set is back in force and the reason is kept.
    {
        PluginHostSupervisor sup(fakeHost, "");
        sup.setRestartCooldownForTesting(100);
        CHECK(sup.setSamples(makeBlob({1, 2})));
        crashHost(sup);
        CHECK(sup.state() == PluginHostSupervisor::State::Failed);
        CHECK(!sup.setSamples(faulty({3, 4, 5}, BlobFault::SlotOutOfRange))); // down: kept, not sent
        CHECK(renderAfterRestart(sup) == 2.0f);
        CHECK(sup.state() == PluginHostSupervisor::State::Running);
        CHECK(contains(sup.lastSamplesError(), "slot 128 is out of range"));
        const auto outcome = sup.samplesOutcome();
        CHECK(!outcome.accepted && outcome.refused);
        crashHost(sup); // and the faulty blob is not sent on a later restart either
        CHECK(renderAfterRestart(sup) == 2.0f);
    }

    // The first blob ever is refused on a fresh host: nothing was accepted, so a restart sends
    // nothing, and the host comes back empty.
    {
        PluginHostSupervisor sup(fakeHost, "");
        sup.setRestartCooldownForTesting(100);
        CHECK(!sup.setSamples(faulty({1, 2, 3}, BlobFault::Truncated)));
        CHECK(renderLevel(sup) == 0.0f);
        const std::uint64_t exchanges = sup.samplesExchangeCount();
        crashHost(sup);
        CHECK(renderAfterRestart(sup) == 0.0f);
        CHECK(sup.samplesExchangeCount() == exchanges); // no samples were sent to the new host
    }

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

// B1. A blob that kills or stalls the host gets one more try on the next host, then it is
// given up and the accepted set plays again. `envName` makes the fake host fail on 3 slots.
void checkBlobThatBreaksTheHost(const char* fakeHost, const char* envName, bool previouslyAccepted)
{
    EnvGuard guard(envName, "3");
    PluginHostSupervisor sup(fakeHost, "");
    sup.setStateTimeoutForTesting(300);
    sup.setRestartCooldownForTesting(100);

    const float inForce = previouslyAccepted ? 2.0f : 0.0f;
    if (previouslyAccepted)
    {
        CHECK(sup.setSamples(makeBlob({1, 2})));
        CHECK(renderLevel(sup) == 2.0f);
    }

    CHECK(!sup.setSamples(makeBlob({4, 5, 6}))); // the host dies or stalls on this one
    CHECK(!sup.lastSamplesError().empty());

    // At most two failed starts, then the accepted set is back.
    CHECK(renderAfterRestart(sup, 15000) == inForce);
    CHECK(sup.state() == PluginHostSupervisor::State::Running);
    CHECK(contains(sup.lastSamplesError(),
                   previouslyAccepted ? "the host failed while loading the new samples; the previous slots were kept"
                                      : "the host failed while loading the new samples; no samples are loaded"));
    const auto outcome = sup.samplesOutcome();
    CHECK(!outcome.accepted && outcome.refused);

    // The bad blob is not sent again: another crash and restart still ends Running.
    crashHost(sup);
    CHECK(renderAfterRestart(sup, 15000) == inForce);
    CHECK(sup.state() == PluginHostSupervisor::State::Running);
}

// A start that fails only because the host is down is not an attempt: the wanted set still
// gets its one try on a live host.
void checkNotSentIsNotAnAttempt(const char* fakeHost)
{
    PluginHostSupervisor sup(fakeHost, ""); // the normal cooldown: all three calls fall inside it
    CHECK(sup.setSamples(makeBlob({1, 2})));
    crashHost(sup);
    for (int i = 0; i < 3; ++i)
        CHECK(!sup.setSamples(makeBlob({4, 5, 6}))); // down or cooling off, every time
    CHECK(renderAfterRestart(sup) == 3.0f);
    CHECK(sup.lastSamplesError().empty());
}

// M2. A host that never reads cannot hold a send for ever.
void checkHostThatNeverReads(const char* fakeHost)
{
    EnvGuard guard("PHYLO_FAKE_NEVER_READ", "1");
    const auto big = bigBlob();

    // The supervisor gives up on the write after the state timeout.
    {
        PluginHostSupervisor sup(fakeHost, "");
        sup.setStateTimeoutForTesting(500);
        const auto t0 = Clock::now();
        CHECK(!sup.setSamples(big));
        CHECK(msSince(t0) < 500 + 2500);
        CHECK(contains(sup.lastSamplesError(), "while sending the samples"));
        CHECK(!contains(sup.lastSamplesError(), "while sending the state"));
        CHECK(sup.state() == PluginHostSupervisor::State::Failed);
    }

    // Destroying a renderer whose worker is in the middle of such a send returns.
    {
        auto sup = std::make_unique<PluginHostSupervisor>(fakeHost, "");
        sup->setStateTimeoutForTesting(1000);
        auto renderer = std::make_unique<PipelinedRenderer>(std::move(sup));
        renderer->sendSamples(big);
        sleepMs(300); // the worker is now writing
        const auto t0 = Clock::now();
        renderer.reset();
        CHECK(msSince(t0) < 1000 + 4000);
    }
}

void checkStrayAnswerAndLongReason(const char* fakeHost)
{
    // m1. The host sends one extra empty StateResult after each refusal. The next bad blob is
    // still refused, not "accepted" by the stray answer.
    {
        EnvGuard guard("PHYLO_FAKE_EXTRA_RESULT", "1");
        PluginHostSupervisor sup(fakeHost, "");
        CHECK(sup.setSamples(makeBlob({1})));
        CHECK(!sup.setSamples(faulty({2, 3}, BlobFault::Truncated)));
        sleepMs(50); // the stray answer is in the pipe by now
        CHECK(!sup.setSamples(faulty({2, 3}, BlobFault::SlotOutOfRange)));
        CHECK(contains(sup.lastSamplesError(), "slot 128 is out of range"));
        sleepMs(50);
        CHECK(!sup.setSamples(faulty({2, 3}, BlobFault::WrongHeader)));
        CHECK(sup.setSamples(makeBlob({4, 5}))); // and a good blob is judged on its own answer
        CHECK(renderLevel(sup) == 2.0f);
    }

    // m2. A very long refusal text is cut at 200 bytes, on a character boundary, with "...".
    {
        EnvGuard guard("PHYLO_FAKE_LONG_REASON", "1");
        PluginHostSupervisor sup(fakeHost, "");
        CHECK(sup.setSamples(makeBlob({1})));
        CHECK(!sup.setSamples(faulty({2}, BlobFault::Truncated)));
        const std::string& reason = sup.lastSamplesError();
        // 'x' plus 99 two-byte characters is 199 bytes; byte 200 would split the next one.
        CHECK(reason.size() == 199 + 3);
        CHECK(reason.size() >= 3 && reason.compare(reason.size() - 3, 3, "...") == 0);
        CHECK(reason.size() >= 4 && static_cast<unsigned char>(reason[reason.size() - 4]) == 0xA9);
        CHECK(renderLevel(sup) == 1.0f);
    }
}

// m6. Bytes that are not a sample blob are refused without being sent.
void checkNonBlobBytes(const char* fakeHost)
{
    PluginHostSupervisor sup(fakeHost, "");
    CHECK(!sup.setSamples({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20}));
    CHECK(sup.lastSamplesError() == "not a sample blob");
    CHECK(sup.state() == PluginHostSupervisor::State::Idle); // not even a host was started for it
    CHECK(!sup.setSamples({}));
    CHECK(sup.lastSamplesError() == "not a sample blob");

    sup.setRestartCooldownForTesting(100);
    CHECK(sup.setSamples(makeBlob({0, 1})));
    CHECK(!sup.setSamples({9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9}));
    crashHost(sup);
    CHECK(renderAfterRestart(sup) == 2.0f); // the accepted set, not the non-blob bytes
}

// m7. On a fresh start a refused plugin state keeps its reason, though samples are sent after it.
void checkRefusedStateKeepsItsReason(const char* fakeHost)
{
    EnvGuard guard("PHYLO_FAKE_REFUSE_STATE", "1");
    PluginHostSupervisor sup(fakeHost, "");
    sup.setRestartCooldownForTesting(100);
    CHECK(sup.setSamples(makeBlob({0, 1})));
    crashHost(sup);
    sleepMs(150); // the cooldown is over: setState starts the host, state first, then samples
    CHECK(!sup.setState({1, 2, 3}));
    CHECK(contains(sup.lastError(), "state not wanted"));
    CHECK(sup.state() == PluginHostSupervisor::State::Running);
    CHECK(renderLevel(sup) == 2.0f); // the samples went through after the refused state

    // On a running host the reason is there as well.
    CHECK(!sup.setState({4, 5, 6}));
    CHECK(contains(sup.lastError(), "state not wanted"));
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
        sleepMs(2);
    }
    return false;
}

// Submits blocks and takes results. Returns the level of the newest block taken, -1 if none.
float pumpOnce(PipelinedRenderer& renderer, std::vector<float>& out)
{
    renderer.submit(block());
    float level = -1.0f;
    while (renderer.take(out, kFrames * kChannels))
        level = out.empty() ? -1.0f : out[0];
    return level;
}

// Keeps blocks flowing until a block reports `level`. False when it takes longer than limitMs.
bool pumpUntilLevel(PipelinedRenderer& renderer, float level, int limitMs)
{
    std::vector<float> out;
    const auto t0 = Clock::now();
    while (msSince(t0) < limitMs)
    {
        if (pumpOnce(renderer, out) == level)
            return true;
        sleepMs(5);
    }
    return false;
}

// Keeps blocks flowing until the status starts with `prefix`.
bool pumpUntilStatus(PipelinedRenderer& renderer, const char* prefix, int limitMs)
{
    std::vector<float> out;
    const auto t0 = Clock::now();
    while (msSince(t0) < limitMs)
    {
        pumpOnce(renderer, out);
        if (renderer.samplesStatus().rfind(prefix, 0) == 0)
            return true;
        sleepMs(5);
    }
    return false;
}

// M1. The status follows the host through a restart.
void checkRendererStatusAfterRestart(const char* fakeHost)
{
    for (int round = 0; round < 2; ++round)
    {
        const bool goodBlob = round == 0;
        auto sup = std::make_unique<PluginHostSupervisor>(fakeHost, "");
        sup->setRestartCooldownForTesting(600);
        PipelinedRenderer renderer(std::move(sup));
        std::vector<float> out;

        renderer.sendSamples(makeBlob({1, 2}));
        CHECK(waitForAnswer(renderer, 5000));
        CHECK(renderer.samplesStatus() == "samples accepted");
        CHECK(pumpUntilLevel(renderer, 2.0f, 3000));

        // Kill the host and keep blocks flowing for a moment, so a block finds the crash and the
        // next send comes inside the restart cooldown.
        renderer.requestKillForTesting();
        for (int i = 0; i < 20; ++i)
        {
            pumpOnce(renderer, out);
            sleepMs(5);
        }

        if (goodBlob)
        {
            renderer.sendSamples(makeBlob({4, 5, 6}));
            CHECK(pumpUntilLevel(renderer, 3.0f, 8000)); // the restart delivers the set
            CHECK(renderer.samplesStatus() == "samples accepted");
        }
        else
        {
            renderer.sendSamples(faulty({4, 5, 6}, BlobFault::SlotOutOfRange));
            CHECK(pumpUntilStatus(renderer, "samples refused: ", 8000));
            CHECK(contains(renderer.samplesStatus(), "slot 128"));
            CHECK(pumpUntilLevel(renderer, 2.0f, 3000)); // and the old set plays
            float lastLevel = -1.0f;
            for (int i = 0; i < 30; ++i)
            {
                const float level = pumpOnce(renderer, out);
                if (level >= 0.0f)
                    lastLevel = level;
                sleepMs(5);
            }
            CHECK(lastLevel == 2.0f);
            CHECK(contains(renderer.samplesStatus(), "slot 128"));
        }
    }
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
            CHECK(msSince(t0) < kPromptMs); // sendSamples, submit and take together
        }
        CHECK(waitForAnswer(renderer, 5000));
        const std::string status = renderer.samplesStatus();
        if (lastIsBad)
            CHECK(status.rfind("samples refused: ", 0) == 0 && contains(status, "truncated"));
        else
            CHECK(status == "samples accepted");
    }

    // Blocks keep flowing while a multi-megabyte blob is on its way, and the host ends up
    // holding that blob's set (4 slots; the set before it had 3).
    {
        const auto big = bigBlob();
        const auto tSend = Clock::now();
        renderer.sendSamples(big);
        CHECK(msSince(tSend) < kPromptMs);
        for (int i = 0; i < 20; ++i)
        {
            const auto t0 = Clock::now();
            renderer.submit(block());
            CHECK(msSince(t0) < kPromptMs);
            const auto t1 = Clock::now();
            renderer.take(out, kFrames * kChannels);
            CHECK(msSince(t1) < kPromptMs);
            sleepMs(2);
        }
        CHECK(pumpUntilLevel(renderer, 4.0f, 5000));
        CHECK(waitForAnswer(renderer, 5000));
        CHECK(renderer.samplesStatus() == "samples accepted");
    }
}

// sendSamples from one thread while another does submit and take: no race, no stall.
void checkSendFromAnotherThread(const char* fakeHost)
{
    PipelinedRenderer renderer(std::make_unique<PluginHostSupervisor>(fakeHost, ""));
    const auto good = makeBlob({1, 2, 3});
    const auto bad = faulty({1, 2}, BlobFault::Truncated);

    std::atomic<bool> stop{false};
    std::thread sender(
        [&]
        {
            int i = 0;
            while (!stop.load())
            {
                renderer.sendSamples(i % 3 == 2 ? bad : good); // no CHECK here: it is not thread-safe
                ++i;
                sleepMs(3);
            }
        });

    std::vector<float> out;
    long long worst = 0;
    const auto start = Clock::now();
    while (msSince(start) < 1000)
    {
        const auto t0 = Clock::now();
        renderer.submit(block());
        renderer.take(out, kFrames * kChannels);
        worst = std::max(worst, msSince(t0));
        sleepMs(1);
    }
    stop.store(true);
    sender.join();
    CHECK(worst < kPromptMs);

    renderer.sendSamples(good);
    CHECK(pumpUntilLevel(renderer, 3.0f, 5000));
    CHECK(waitForAnswer(renderer, 5000));
    CHECK(renderer.samplesStatus() == "samples accepted");
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

    // A hang is a failure too (a send that never times out). Do not wait for the CI limit.
    std::thread([]
                {
                    std::this_thread::sleep_for(std::chrono::seconds(120));
                    std::printf("FAIL: the check did not finish in 120 s\n");
                    std::fflush(stdout);
                    std::_Exit(1);
                })
        .detach();

    checkSupervisorWithFake(argv[1]);
    checkBlobThatBreaksTheHost(argv[1], "PHYLO_FAKE_DIE_ON_SLOTS", true);
    checkBlobThatBreaksTheHost(argv[1], "PHYLO_FAKE_DIE_ON_SLOTS", false);
    checkBlobThatBreaksTheHost(argv[1], "PHYLO_FAKE_STALL_ON_SLOTS", true);
    checkBlobThatBreaksTheHost(argv[1], "PHYLO_FAKE_STALL_ON_SLOTS", false);
    checkNotSentIsNotAnAttempt(argv[1]);
    checkHostThatNeverReads(argv[1]);
    checkStrayAnswerAndLongReason(argv[1]);
    checkNonBlobBytes(argv[1]);
    checkRefusedStateKeepsItsReason(argv[1]);
    checkRendererWithFake(argv[1]);
    checkRendererStatusAfterRestart(argv[1]);
    checkSendFromAnotherThread(argv[1]);
    checkRealHost(argv[2]);

    std::printf("sample send check: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
