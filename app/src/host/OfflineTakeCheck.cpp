// Headless check of the offline take renderer (M4). Plain C++, no JUCE.
//
// Usage: phylo_offline_take_check [path to phylo-plughost]
//
// Checks that:
//  - a 4-beat pattern at 120 BPM gives note-ons at frames 0, 24000, 48000, 72000,
//  - the note-on frames and the audio size do not change with the block size,
//  - the renderer is called once per block and never sees a MIDI offset past its block,
//  - a renderer that returns nothing gives silence of the full length, and
//  - (with a host path) setState with no plugin loaded refuses cleanly and returns.

#include "OfflineTake.h"
#include "PluginHostSupervisor.h"

#include <chrono>
#include <cstdio>

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

phylo::Pattern fourBeats()
{
    phylo::Pattern p(4.0);
    for (int b = 0; b < 4; ++b)
        p.addNote(static_cast<double>(b), 60, 100, 0.5);
    return p;
}

constexpr double kBpm = 120.0;
constexpr int kRate = 48000;
constexpr std::size_t kFrames = 2 * kRate; // two seconds, four beats

// Renders silence and records the block sizes and MIDI offsets it was given.
BlockRenderer silentRecorder(std::vector<std::size_t>& sizes, bool& midiInRange)
{
    return [&sizes, &midiInRange](const phylo::host::ProcessRequest& in, std::vector<float>& out) {
        sizes.push_back(in.frames);
        for (const auto& m : in.midi)
            if (m.frame >= in.frames)
                midiInRange = false;
        out.assign(static_cast<std::size_t>(in.frames) * in.channels, 0.0f);
    };
}
} // namespace

int main(int argc, char** argv)
{
    const auto pattern = fourBeats();

    // 1. Note-on frames at 48-frame blocks.
    std::vector<std::size_t> sizes48;
    bool inRange48 = true;
    auto take48 = renderPatternOffline(pattern, kBpm, kRate, kFrames, silentRecorder(sizes48, inRange48), 48);
    const std::vector<std::size_t> expected = { 0, 24000, 48000, 72000 };
    CHECK(take48.noteOnFrames == expected);

    // 2. The same take at 512-frame blocks has the same note-on frames and length.
    std::vector<std::size_t> sizes512;
    bool inRange512 = true;
    auto take512 = renderPatternOffline(pattern, kBpm, kRate, kFrames, silentRecorder(sizes512, inRange512), 512);
    CHECK(take512.noteOnFrames == take48.noteOnFrames);
    CHECK(take512.stereo.size() == take48.stereo.size());

    // 3. The renderer got one call per block, the blocks cover the take, and MIDI stays in range.
    std::size_t total = 0;
    for (std::size_t n : sizes48)
        total += n;
    CHECK(total == kFrames);
    CHECK(sizes48.size() == kFrames / 48);
    CHECK(sizes512.size() == kFrames / 512 + ((kFrames % 512) ? 1 : 0));
    CHECK(inRange48);
    CHECK(inRange512);

    // 4. Silence in, silence out, at the full length.
    CHECK(take48.stereo.size() == kFrames * 2);
    bool allZero = true;
    for (float s : take48.stereo)
        if (s != 0.0f)
            allZero = false;
    CHECK(allZero);

    // 5. Optional: a host with no plugin refuses a state and does not hang.
    if (argc >= 2)
    {
        PluginHostSupervisor sup(argv[1], "");
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = sup.setState({ 1, 2, 3 });
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        CHECK(!ok);
        CHECK(ms < PluginHostSupervisor::kStateTimeoutMs);
        std::printf("setState without a plugin: ok=%d after %lld ms, error=\"%s\"\n", ok ? 1 : 0,
                    static_cast<long long>(ms), sup.lastError().c_str());
    }

    if (failures == 0)
        std::printf("offline take check: all passed\n");
    else
        std::printf("offline take check: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
