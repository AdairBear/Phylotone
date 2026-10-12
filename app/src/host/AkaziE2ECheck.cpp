// End-to-end check of sample delivery against a real Akazi XL VST3 (docs/SAMPLE_INTERFACE.md,
// "Delivery" and "The answer"). Plain C++, no JUCE: it drives the real phylo-plughost through
// PluginHostSupervisor, the same code the app uses.
//
// Usage: phylo_akazi_e2e_check <phylo-plughost> <plugin.vst3> <mode>
//   mode "sound"   a build with sample-set support: the full list of checks below
//   mode "old"     a build without it: delivery runs, and the answer is "did not take"
//   mode "budget"  a set of about 60 MiB: wall time and the host's peak memory
//                  (its own process, so the peak is that of the host of this run only)
//
// Registered in ctest only when the CMake cache variable PHYLO_AKAZI_VST3 is set.
//
// "sound" checks, with a sine sample in slot 0 (root note 60):
//  1. before any set the note is silent; after a valid set the plugin answers empty and the
//     note (MIDI 60, channel 1) sounds at the sample's pitch,
//  2. a two-slot set, then Program Change 1: the other sample plays,
//  3. a blob the host refuses (slot 128) comes back with its reason; the sample still sounds,
//  4. with PHYLO_HOST_SKIP_BLOB_CHECK=1 each bad blob reaches the plugin and the plugin's own
//     reason comes back; the sample still sounds,
//  5. a set with no slots is answered empty and the note is silent again,
//  6. plain XML state via GetState and SetState round-trips, and a set sent after it is
//     still answered.
//
// The check reports what it saw. It does not retry to get a pass.

#include "PluginHostSupervisor.h"
#include "ProcessPipe.h"
#include "phylo/host/Wire.h"
#include "phylo/samples/SampleBlob.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
  #include <sys/resource.h>
#endif

namespace
{
using namespace phylo::samples;
using Clock = std::chrono::steady_clock;
using Bytes = std::vector<std::uint8_t>;

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

constexpr std::uint32_t kBlock = 1024;
constexpr std::uint32_t kChannels = 2;
constexpr double kRate = 48000.0;
constexpr float kSilence = 1e-4f;   // peak below this is silence
constexpr float kAudible = 0.02f;   // peak above this is sound
const char kDidNotTake[] = "the plugin did not take the sample set";

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
        _putenv_s(name, "");
#else
        unsetenv(name);
#endif
    }
    EnvGuard(const EnvGuard&) = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;
    const char* name;
};

long long msSince(Clock::time_point t0)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
}

void sleepMs(int ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// What a played note sounded like: channel 0 only.
struct Heard
{
    float peak = 0.0f;
    double hz = 0.0; // estimated from rising zero crossings; 0 when too quiet to tell
    bool rendered = false;
};

Heard analyse(const std::vector<float>& ch0)
{
    Heard h;
    h.rendered = !ch0.empty();
    for (float f : ch0)
        h.peak = std::max(h.peak, std::fabs(f));
    if (h.peak < kAudible)
        return h;
    int crossings = 0;
    for (std::size_t i = 1; i < ch0.size(); ++i)
        if (ch0[i - 1] < 0.0f && ch0[i] >= 0.0f)
            ++crossings;
    h.hz = crossings * kRate / static_cast<double>(ch0.size());
    return h;
}

phylo::host::ProcessRequest blockWith(const std::vector<phylo::host::WireMidi>& midi)
{
    phylo::host::ProcessRequest r;
    r.frames = kBlock;
    r.channels = kChannels;
    r.midi = midi;
    r.audio.assign(static_cast<std::size_t>(kBlock) * kChannels, 0.0f);
    return r;
}

phylo::host::WireMidi noteOn(int note) { return {0, 0x90, static_cast<std::uint8_t>(note), 100}; }
phylo::host::WireMidi noteOff(int note) { return {0, 0x80, static_cast<std::uint8_t>(note), 0}; }
phylo::host::WireMidi allNotesOff() { return {0, 0xB0, 123, 0}; }
phylo::host::WireMidi programChange(int n) { return {0, 0xC0, static_cast<std::uint8_t>(n), 0}; }

// Renders `blocks` blocks, the first with `first` as its MIDI. Returns channel 0.
std::vector<float> run(PluginHostSupervisor& sup, const std::vector<phylo::host::WireMidi>& first, int blocks)
{
    std::vector<float> ch0;
    for (int b = 0; b < blocks; ++b)
    {
        std::vector<float> out;
        sup.render(blockWith(b == 0 ? first : std::vector<phylo::host::WireMidi>{}), out);
        if (out.size() != static_cast<std::size_t>(kBlock) * kChannels)
            return {};
        ch0.insert(ch0.end(), out.begin(), out.begin() + kBlock);
    }
    return ch0;
}

// Plays MIDI note `note` on channel 1 for about 190 ms, then releases it and lets the tail die.
Heard play(PluginHostSupervisor& sup, int note = 60)
{
    const Heard h = analyse(run(sup, {noteOn(note)}, 9));
    run(sup, {noteOff(note), allNotesOff()}, 30);
    return h;
}

SampleSlot sine(int slot, double hz, double seconds, float amp = 0.5f, int channels = 1, std::uint8_t root = 60)
{
    SampleSlot s;
    s.slot = slot;
    s.name = "sine " + std::to_string(static_cast<int>(hz));
    s.rootNote = root;
    s.channels = channels;
    const std::size_t frames = static_cast<std::size_t>(seconds * kRate);
    s.samples.resize(frames * static_cast<std::size_t>(channels));
    for (std::size_t i = 0; i < frames; ++i)
    {
        const float v = amp * static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * hz * static_cast<double>(i) / kRate));
        for (int c = 0; c < channels; ++c)
            s.samples[i * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)] = v;
    }
    return s;
}

Bytes encode(const SampleSlotSet& slots)
{
    Bytes blob;
    const BlobResult r = encodeSampleBlob(slots, blob);
    CHECK(r.ok());
    return blob;
}

bool near(double hz, double want)
{
    return hz > want * 0.85 && hz < want * 1.15;
}

void report(const char* what, const Heard& h)
{
    std::printf("  %s: peak %.3f, about %.0f Hz\n", what, static_cast<double>(h.peak), h.hz);
}

// ---------------------------------------------------------------------------------------

void checkOldPlugin(const char* host, const char* plugin)
{
    std::printf("old plugin: delivery runs, the answer is \"did not take\"\n");
    PluginHostSupervisor sup(host, plugin);
    sup.setStateTimeoutForTesting(20000);
    CHECK(play(sup).peak < kSilence);
    CHECK(sup.state() == PluginHostSupervisor::State::Running);

    const auto blob = encode({sine(0, 440, 1.0)});
    CHECK(!sup.setSamples(blob));
    std::printf("  answer: \"%s\"\n", sup.lastSamplesError().c_str());
    CHECK(sup.lastSamplesError() == kDidNotTake);
    CHECK(sup.state() == PluginHostSupervisor::State::Running);
    CHECK(play(sup).peak < kSilence); // nothing was loaded

    // The host check is still first: a bad blob gets its own reason, not "did not take".
    auto bad = blob;
    CHECK(applyBlobFaultForTesting(bad, BlobFault::SlotOutOfRange));
    CHECK(!sup.setSamples(bad));
    std::printf("  host refusal: \"%s\"\n", sup.lastSamplesError().c_str());
    CHECK(sup.lastSamplesError().find("128") != std::string::npos);
    CHECK(sup.state() == PluginHostSupervisor::State::Running);
}

// Step 6, with the wire protocol used directly because the supervisor has no GetState.
bool readFrame(ProcessPipe& pipe, phylo::host::FrameDecoder& dec, phylo::host::MsgType want,
               phylo::host::Frame& out, int timeoutMs)
{
    const auto t0 = Clock::now();
    std::uint8_t chunk[8192];
    for (;;)
    {
        while (dec.next(out))
            if (out.type == want)
                return true;
        if (dec.failed() || msSince(t0) > timeoutMs)
            return false;
        const long n = pipe.readWithTimeout(chunk, sizeof chunk, 50);
        if (n < 0)
            return false;
        if (n > 0)
            dec.feed(chunk, static_cast<std::size_t>(n));
    }
}

void checkPlainStateThenSet(const char* host, const char* plugin, bool expectSound)
{
    std::printf("plain XML state through GetState and SetState, then a set\n");
    using phylo::host::MsgType;
    ProcessPipe pipe;
    phylo::host::FrameDecoder dec;
    phylo::host::Frame f;
    CHECK(pipe.start(host, {}));
    CHECK(readFrame(pipe, dec, MsgType::Hello, f, 10000));
    const auto send = [&pipe](MsgType t, const Bytes& payload)
    {
        const auto bytes = phylo::host::encodeFrame(t, payload);
        return pipe.writeAllWithTimeout(bytes.data(), bytes.size(), 20000);
    };
    CHECK(send(MsgType::Load, phylo::host::encodeText(plugin)));
    CHECK(readFrame(pipe, dec, MsgType::Loaded, f, 20000));
    std::string loadError;
    phylo::host::decodeText(f.payload, loadError);
    CHECK(loadError.empty());

    CHECK(send(MsgType::GetState, {}));
    CHECK(readFrame(pipe, dec, MsgType::State, f, 10000));
    const Bytes state1 = f.payload;
    CHECK(!state1.empty());

    CHECK(send(MsgType::SetState, state1));
    CHECK(readFrame(pipe, dec, MsgType::StateResult, f, 20000));
    std::string answer;
    phylo::host::decodeText(f.payload, answer);
    CHECK(answer.empty()); // plain state keeps today's path

    CHECK(send(MsgType::GetState, {}));
    CHECK(readFrame(pipe, dec, MsgType::State, f, 10000));
    CHECK(f.payload == state1); // restoring it changed nothing

    // A set sent after the plain state is still answered.
    const auto blob = encode({sine(0, 440, 0.5)});
    CHECK(send(MsgType::SetState, blob));
    CHECK(readFrame(pipe, dec, MsgType::StateResult, f, 20000));
    phylo::host::decodeText(f.payload, answer);
    std::printf("  answer to the set after plain state: \"%s\"\n", answer.c_str());
    CHECK(expectSound ? answer.empty() : answer == kDidNotTake);

    // And plain state again afterwards.
    CHECK(send(MsgType::SetState, state1));
    CHECK(readFrame(pipe, dec, MsgType::StateResult, f, 20000));
    phylo::host::decodeText(f.payload, answer);
    CHECK(answer.empty());
    pipe.kill();
}

// Leaving the host. Shutdown, then the input closing a few ms later, and frames in the same
// write after Shutdown, must always end in a clean exit with code 0: the reader and audio
// threads must never be left running on objects that are gone. Repeated, because the fault
// this guards against was a race that lost only some of the time.
void checkHostLeavesCleanly(const char* host, const char* plugin, int rounds)
{
    using phylo::host::MsgType;
    int clean = 0;
    for (int i = 0; i < rounds; ++i)
    {
        ProcessPipe pipe;
        CHECK(pipe.start(host, {}));
        phylo::host::FrameDecoder dec;
        phylo::host::Frame f;
        std::uint8_t chunk[4096];
        const auto waitFor = [&](MsgType want)
        {
            const auto t0 = Clock::now();
            while (msSince(t0) < 20000)
            {
                while (dec.next(f))
                    if (f.type == want)
                        return true;
                const long n = pipe.readWithTimeout(chunk, sizeof chunk, 50);
                if (n < 0)
                    return false;
                if (n > 0)
                    dec.feed(chunk, static_cast<std::size_t>(n));
            }
            return false;
        };
        const auto bytesOf = [](MsgType t, const std::vector<std::uint8_t>& p) { return phylo::host::encodeFrame(t, p); };
        CHECK(waitFor(MsgType::Hello));
        if (plugin != nullptr && plugin[0] != 0)
        {
            const auto load = bytesOf(MsgType::Load, phylo::host::encodeText(plugin));
            CHECK(pipe.writeAllWithTimeout(load.data(), load.size(), 20000));
            CHECK(waitFor(MsgType::Loaded));
        }

        std::vector<std::uint8_t> out = bytesOf(MsgType::Shutdown, {});
        const int kind = i % 3;
        if (kind == 1)
        {
            // Frames after Shutdown in the same write: audio, a state request, a sample set.
            const auto req = bytesOf(MsgType::Process, phylo::host::encodeProcess(blockWith({})));
            const auto get = bytesOf(MsgType::GetState, {});
            const auto set = bytesOf(MsgType::SetState, encode({sine(0, 440, 0.2)}));
            for (const auto* extra : {&req, &req, &get, &set, &req})
                out.insert(out.end(), extra->begin(), extra->end());
        }
        else if (kind == 2)
        {
            // Busy audio traffic, then the input just closes with no Shutdown at all. Nobody
            // reads the answers, so the host's output pipe fills and its audio thread is
            // stuck in a write, holding the plugin, when the host has to leave.
            out.clear();
            const auto req = bytesOf(MsgType::Process, phylo::host::encodeProcess(blockWith({})));
            for (int k = 0; k < 20; ++k)
                out.insert(out.end(), req.begin(), req.end());
        }
        CHECK(pipe.writeAllWithTimeout(out.data(), out.size(), 20000));
        sleepMs(i % 5); // 0 to 4 ms: the window where the race lived
        const int code = pipe.closeInputAndWait(10000);
        if (code == 0)
            ++clean;
        else
            std::printf("  round %d (kind %d): exit code %d\n", i, kind, code);
    }
    CHECK(clean == rounds);
}

void checkSound(const char* host, const char* plugin)
{
    std::printf("sound: a set loads, plays, switches, refuses and clears\n");
    PluginHostSupervisor sup(host, plugin);
    sup.setStateTimeoutForTesting(20000);

    // 1. Silent before, audible after, at the sample's pitch.
    Heard before = play(sup);
    report("before any set", before);
    CHECK(before.rendered);
    CHECK(before.peak < kSilence);
    CHECK(sup.state() == PluginHostSupervisor::State::Running);

    CHECK(sup.setSamples(encode({sine(0, 440, 1.0)})));
    std::printf("  answer: \"%s\"\n", sup.lastSamplesError().c_str());
    CHECK(sup.lastSamplesError().empty());
    Heard first = play(sup);
    report("after a one-slot set", first);
    CHECK(first.peak > kAudible);
    CHECK(near(first.hz, 440));

    // 2. Two slots; slot 0 is active after the set; Program Change 1 selects the other.
    CHECK(sup.setSamples(encode({sine(0, 440, 1.0), sine(1, 1000, 1.0)})));
    CHECK(sup.lastSamplesError().empty());
    Heard slot0 = play(sup);
    report("two-slot set, slot 0", slot0);
    CHECK(near(slot0.hz, 440));

    run(sup, {programChange(1)}, 1);
    const auto t0 = Clock::now();
    Heard slot1;
    int polls = 0;
    for (; polls < 40; ++polls) // the plugin applies it from a timer: wait, but not for ever
    {
        sleepMs(25);
        slot1 = play(sup);
        if (near(slot1.hz, 1000))
            break;
    }
    report("after Program Change 1", slot1);
    std::printf("  Program Change took effect after %lld ms (%d polls)\n", msSince(t0), polls + 1);
    CHECK(near(slot1.hz, 1000));

    // 3. The host refuses a blob (slot 128). The sample that was playing still plays.
    auto bad = encode({sine(0, 440, 1.0), sine(1, 1000, 1.0)});
    CHECK(applyBlobFaultForTesting(bad, BlobFault::SlotOutOfRange));
    CHECK(!sup.setSamples(bad));
    std::printf("  host refusal: \"%s\"\n", sup.lastSamplesError().c_str());
    CHECK(sup.lastSamplesError().find("128") != std::string::npos);
    CHECK(sup.lastSamplesError() != kDidNotTake);
    Heard afterHostRefusal = play(sup);
    report("after a host refusal", afterHostRefusal);
    CHECK(near(afterHostRefusal.hz, 1000));

    // 5. No slots: valid, answered empty, everything cleared, silent.
    CHECK(sup.setSamples(encode({})));
    CHECK(sup.lastSamplesError().empty());
    Heard cleared = play(sup);
    report("after a set with no slots", cleared);
    CHECK(cleared.peak < kSilence);
}

// 4. Needs its own host: the variable is read by the host process when a set arrives.
void checkPluginOwnRefusals(const char* host, const char* plugin)
{
    std::printf("bad blobs that reach the plugin (PHYLO_HOST_SKIP_BLOB_CHECK=1)\n");
    EnvGuard skip("PHYLO_HOST_SKIP_BLOB_CHECK", "1");
    PluginHostSupervisor sup(host, plugin);
    sup.setStateTimeoutForTesting(20000);

    CHECK(sup.setSamples(encode({sine(0, 440, 1.0)})));
    Heard good = play(sup);
    report("good set", good);
    CHECK(near(good.hz, 440));

    struct Fault { BlobFault fault; const char* name; };
    const Fault faults[] = {{BlobFault::WrongHeader, "wrong header"},
                            {BlobFault::WrongSampleRate, "wrong sample rate"},
                            {BlobFault::SlotOutOfRange, "slot 128"},
                            {BlobFault::SampleTooLong, "61 s sample"},
                            {BlobFault::Truncated, "truncated"}};
    for (const Fault& k : faults)
    {
        auto bad = encode({sine(0, 440, 1.0)});
        CHECK(applyBlobFaultForTesting(bad, k.fault));
        CHECK(!sup.setSamples(bad));
        const std::string reason = sup.lastSamplesError();
        std::printf("  %s: plugin says \"%s\"\n", k.name, reason.c_str());
        CHECK(!reason.empty());
        CHECK(reason != kDidNotTake);
        CHECK(sup.state() == PluginHostSupervisor::State::Running);
        Heard still = play(sup);
        CHECK(near(still.hz, 440)); // the previous slots were kept
    }
}

// 7. About 60 MiB: five 60 s mono samples and one of 27 s.
void checkBudget(const char* host, const char* plugin)
{
    std::printf("full budget: a set of about 60 MiB\n");
    SampleSlotSet slots;
    for (int i = 0; i < 5; ++i)
        slots.push_back(sine(i, 220.0 * (i + 1), 60.0, 0.4f));
    slots.push_back(sine(5, 1320, 27.0, 0.4f));
    const Bytes blob = encode(slots);
    std::printf("  blob: %zu bytes (%.1f MiB)\n", blob.size(), static_cast<double>(blob.size()) / (1024.0 * 1024.0));
    CHECK(blob.size() > 58u * 1024u * 1024u);
    CHECK(blob.size() <= phylo::host::kMaxPayloadBytes);

    long long ms = 0;
    {
        PluginHostSupervisor sup(host, plugin);
        sup.setStateTimeoutForTesting(120000);
        CHECK(play(sup).peak < kSilence); // starts the host and loads the plugin first

        const auto t0 = Clock::now();
        const bool ok = sup.setSamples(blob);
        ms = msSince(t0);
        std::printf("  setSamples: %s in %lld ms, answer \"%s\"\n", ok ? "accepted" : "NOT accepted", ms,
                    sup.lastSamplesError().c_str());
        CHECK(ok);

        const Heard h = play(sup);
        report("slot 0 after the big set", h);
        CHECK(near(h.hz, 220));

        run(sup, {programChange(5)}, 1);
        const auto s0 = Clock::now();
        Heard last;
        int polls = 0;
        for (; polls < 100; ++polls)
        {
            sleepMs(25);
            last = play(sup);
            if (near(last.hz, 1320))
                break;
        }
        std::printf("  switch to slot 5: %lld ms (%d polls), about %.0f Hz\n", msSince(s0), polls + 1, last.hz);
        CHECK(near(last.hz, 1320));
        CHECK(sup.state() == PluginHostSupervisor::State::Running);
    } // the supervisor ends and reaps the host here

#if !defined(_WIN32)
    struct rusage usage;
    if (getrusage(RUSAGE_CHILDREN, &usage) == 0)
    {
#if defined(__APPLE__)
        const double mib = static_cast<double>(usage.ru_maxrss) / (1024.0 * 1024.0); // bytes
#else
        const double mib = static_cast<double>(usage.ru_maxrss) / 1024.0; // kilobytes
#endif
        std::printf("  host peak memory: %.0f MiB\n", mib);
    }
#else
    std::printf("  host peak memory: not measured on this platform\n");
#endif
    std::printf("  wall time to deliver the set: %lld ms\n", ms);
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        std::printf("usage: phylo_akazi_e2e_check <phylo-plughost> <plugin.vst3> <sound|old|budget>\n");
        return 2;
    }
    std::thread([]
                {
                    std::this_thread::sleep_for(std::chrono::seconds(600));
                    std::printf("FAIL: the check did not finish in 600 s\n");
                    std::fflush(stdout);
                    std::_Exit(1);
                })
        .detach();

    const std::string mode = argv[3];
    if (mode == "sound")
    {
        checkSound(argv[1], argv[2]);
        checkPluginOwnRefusals(argv[1], argv[2]);
        checkPlainStateThenSet(argv[1], argv[2], true);
        checkHostLeavesCleanly(argv[1], argv[2], 30);
    }
    else if (mode == "old")
    {
        checkOldPlugin(argv[1], argv[2]);
        checkPlainStateThenSet(argv[1], argv[2], false);
        checkHostLeavesCleanly(argv[1], argv[2], 15);
    }
    else if (mode == "budget")
    {
        checkBudget(argv[1], argv[2]);
    }
    else
    {
        std::printf("unknown mode %s\n", mode.c_str());
        return 2;
    }

    std::printf("akazi e2e check (%s): %d checks, %d failed\n", mode.c_str(), checks, failures);
    return failures == 0 ? 0 : 1;
}
