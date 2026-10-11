// Runs one plugin in a phylo-plughost child process and renders through it (M4).
//
// If the host dies or stops answering, render() returns silence for that block,
// and the host is restarted after a cooldown. The app keeps running either way.
//
// Limits in this slice: each render() waits for the host's answer, so this is
// synchronous and a slow plugin stalls the caller for up to kReplyTimeoutMs. The
// playback integration needs a pipelined version before it is used live.
#pragma once

#include "ProcessPipe.h"
#include "phylo/host/Wire.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

class PluginHostSupervisor
{
public:
    enum class State { Idle, Running, Failed };

    static constexpr int kReplyTimeoutMs = 250;
    static constexpr int kRestartCooldownMs = 1000;
    static constexpr int kStateTimeoutMs = 5000; // restoring a state can be slow
    // Starting the host and loading a plugin is slow (JUCE init, plugin scan), so it gets
    // its own, longer timeout. Audio renders keep the short kReplyTimeoutMs.
    static constexpr int kStartTimeoutMs = 5000;

    PluginHostSupervisor(std::string hostExecutable, std::string pluginPath);

    // Renders one block. `in.audio` is planar input of channels x frames. `out`
    // receives planar output of the same size, silence if the host is unavailable.
    void render(const phylo::host::ProcessRequest& in, std::vector<float>& out);

    // Restores the plugin's state from raw bytes (for example a saved state for Akazi XL).
    // The bytes are kept: every time the host starts, including after a crash and restart,
    // they are sent again before the first render. Returns false, with lastError() set, if
    // the host is unavailable or refused the state. The host stays up when it refuses; only
    // a broken or silent host is marked failed.
    bool setState(const std::vector<std::uint8_t>& bytes);

    // Sends an `akazi.samples.v1` blob (phylo/samples/SampleBlob.h), the full slot set. Kept
    // apart from setState: the two are stored separately and both are sent again after every
    // host start, the plugin state first, then the samples.
    //
    // Returns true when the host answered with an empty StateResult. On a refusal the reason
    // is in lastSamplesError(), the host stays up, and the set that was accepted before stays
    // the one in force: it is what gets sent again after a restart. When the host is not
    // available the blob is kept and sent on the next start; the call returns false with the
    // reason in lastSamplesError().
    bool setSamples(const std::vector<std::uint8_t>& blob);

    // Why the last setSamples (or the re-send after a start) failed. Empty when it was accepted.
    const std::string& lastSamplesError() const noexcept { return samplesError_; }

    State state() const noexcept { return state_; }
    const std::string& lastError() const noexcept { return error_; }

    // Kills the host as a crash would. Used by the kill test.
    void killHostForTesting();

private:
    // Starts the host and loads the plugin. Returns true when the host is ready.
    bool ensureRunning();
    void fail(const std::string& why);

    // Sends one state frame to a running host and waits for the answer.
    bool sendState(const std::vector<std::uint8_t>& bytes);

    // Reads frames until one of `type` arrives, or the timeout passes.
    bool awaitFrame(phylo::host::MsgType type, int timeoutMs, phylo::host::Frame& out);

    std::string hostExecutable_;
    std::string pluginPath_;
    ProcessPipe pipe_;
    phylo::host::FrameDecoder decoder_;
    State state_ = State::Idle;
    std::string error_;
    std::chrono::steady_clock::time_point retryAt_{};
    std::vector<std::uint8_t> restoredState_; // sent on every host start
    bool stateAccepted_ = true;               // the last state the host answered

    // Samples. `wantedSamples_` is the latest set asked for; `acceptedSamples_` is the last
    // one the host accepted. A refusal drops the wanted set back to the accepted one.
    std::vector<std::uint8_t> wantedSamples_;
    std::vector<std::uint8_t> acceptedSamples_;
    bool haveWantedSamples_ = false;   // an empty blob is never valid, but "nothing asked" is
    bool haveAcceptedSamples_ = false;
    std::string samplesError_;
};
