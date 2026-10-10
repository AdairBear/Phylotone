// Kill test for the plugin host supervisor (M4 accept check, without a real plugin).
//
// Usage: phylo_supervisor_check <path to phylo-plughost>
//
// Checks that a host which is killed:
//  - makes render() return silence, without hanging,
//  - does not restart before the cooldown, and
//  - comes back on its own, rendering again, after the cooldown.
// The test process itself keeps running throughout. That is the point.

#include "PluginHostSupervisor.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
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

bool allZero(const std::vector<float>& v)
{
    for (float f : v)
        if (f != 0.0f)
            return false;
    return true;
}

phylo::host::ProcessRequest block()
{
    phylo::host::ProcessRequest r;
    r.frames = 64;
    r.channels = 2;
    r.audio.assign(128, 0.0f);
    return r;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: phylo_supervisor_check <phylo-plughost>\n");
        return 2;
    }

    PluginHostSupervisor sup(argv[1], ""); // no plugin: the host answers with silence
    std::vector<float> out;

    for (int i = 0; i < 5; ++i)
    {
        sup.render(block(), out);
        CHECK(out.size() == 128);
        CHECK(allZero(out));
    }
    CHECK(sup.state() == PluginHostSupervisor::State::Running);

    // Kill the host as a crash would.
    sup.killHostForTesting();
    const auto t0 = std::chrono::steady_clock::now();
    sup.render(block(), out);
    const auto took = std::chrono::steady_clock::now() - t0;
    CHECK(out.size() == 128 && allZero(out));
    CHECK(sup.state() == PluginHostSupervisor::State::Failed);
    CHECK(took < std::chrono::milliseconds(PluginHostSupervisor::kReplyTimeoutMs + 500));

    // Inside the cooldown, no restart: still silent and still failed.
    sup.render(block(), out);
    CHECK(allZero(out));
    CHECK(sup.state() == PluginHostSupervisor::State::Failed);

    // After the cooldown, the host restarts and renders again.
    std::this_thread::sleep_for(std::chrono::milliseconds(PluginHostSupervisor::kRestartCooldownMs + 100));
    sup.render(block(), out);
    CHECK(out.size() == 128 && allZero(out));
    CHECK(sup.state() == PluginHostSupervisor::State::Running);

    // Plugin state is kept and sent again after a restart. With no plugin the host refuses,
    // and the refusal text shows the state was sent: it comes back after the restart.
    const std::vector<std::uint8_t> state { 1, 2, 3 };
    CHECK(!sup.setState(state));
    CHECK(sup.lastError().find("state refused") != std::string::npos);
    sup.killHostForTesting();
    sup.render(block(), out); // the crash is found here: Failed, silent
    CHECK(sup.state() == PluginHostSupervisor::State::Failed);
    std::this_thread::sleep_for(std::chrono::milliseconds(PluginHostSupervisor::kRestartCooldownMs + 100));
    sup.render(block(), out); // restart: the state is sent again before the render
    CHECK(sup.state() == PluginHostSupervisor::State::Running);
    CHECK(sup.lastError().find("state refused") != std::string::npos);

    if (failures == 0)
        std::printf("supervisor kill test: ok\n");
    return failures == 0 ? 0 : 1;
}
