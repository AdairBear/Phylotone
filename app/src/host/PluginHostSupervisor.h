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
    // is in lastSamplesError() (cut to kMaxReasonBytes), the host stays up, and the set that
    // was accepted before stays the one in force: it is what gets sent again after a restart.
    // Bytes that do not even start like a sample blob are refused here, without sending.
    //
    // When the host is not available (down, or in its restart cooldown) the blob is kept and
    // sent on the next start; the call returns false with the reason in lastSamplesError(). That
    // is not an attempt. A blob that was sent to a live host and then ended the exchange
    // without an answer (pipe broke, host died, no reply in time) was not judged: it gets
    // exactly one more try on the next fresh host. If that try also gets no answer the blob is
    // given up: the wanted set goes back to the accepted one (or to none), lastSamplesError()
    // says "the host failed while loading the new samples; ..." and the next start sends the
    // accepted set, so the previous slots play again.
    bool setSamples(const std::vector<std::uint8_t>& blob);

    // Why the last setSamples (or the re-send after a start) failed. Empty when it was
    // accepted. It stays after a restart that only sends the already accepted set again.
    const std::string& lastSamplesError() const noexcept { return samplesError_; }

    // The longest refusal text kept from the host, in bytes (a "..." is added when cut).
    static constexpr std::size_t kMaxReasonBytes = 200;

    // Counts samples exchanges that have finished, whether in setSamples or in the re-send
    // after a host start. A caller that shows the outcome compares this with the last value
    // it saw and reads samplesOutcome() when it changed. Worker thread only, like the rest.
    std::uint64_t samplesExchangeCount() const noexcept { return samplesExchanges_; }

    // The outcome of the latest samples exchange: `error` is empty when accepted; `refused`
    // tells a refusal (the host answered, or the set was given up) from "not sent" (no host
    // answered).
    struct SamplesOutcome
    {
        bool accepted = true;
        bool refused = false;
        std::string error;
    };
    SamplesOutcome samplesOutcome() const;

    // Test hook: shortens the wait for a StateResult and the write of a SetState frame.
    void setStateTimeoutForTesting(int ms) noexcept { stateTimeoutMs_ = ms; }
    // Test hook: shortens the wait before a failed host is started again.
    void setRestartCooldownForTesting(int ms) noexcept { cooldownMs_ = ms; }

    State state() const noexcept { return state_; }
    const std::string& lastError() const noexcept { return error_; }

    // Kills the host as a crash would. Used by the kill test.
    void killHostForTesting();

private:
    // Starts the host and loads the plugin. Returns true when the host is ready.
    bool ensureRunning();
    void fail(const std::string& why);

    // Sends one state frame to a running host and waits for the answer. A refusal leaves its
    // reason in stateError_ and error_.
    bool sendState(const std::vector<std::uint8_t>& bytes);

    // Throws away frames that are already buffered or readable at once, so an old StateResult
    // cannot be taken as the answer to the next request.
    void discardStaleFrames();

    // Notes that a samples exchange ended without an answer, and gives the wanted set up when
    // this was its second such try.
    void samplesExchangeBroken(const std::string& text);

    // What a SetState exchange came to. Broken means the pipe or the answer failed and the
    // host has been marked failed; Refused means the host answered with a reason and is fine.
    enum class StateReply { Accepted, Refused, Broken };

    // The one place a SetState frame is written and its StateResult awaited. `text` gets the
    // host's refusal reason (cut short), or the failure text when Broken. `what` ("state" or
    // "samples") is for the failure text.
    StateReply exchangeState(const std::vector<std::uint8_t>& bytes, std::string& text, const char* what);

    // Sends the wanted samples to a host that has just started. If the host refuses them,
    // the accepted set is sent instead. Returns false only when the host broke.
    bool sendWantedSamples();

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
    std::string stateError_;                  // why the last state was refused; apart from error_
    int stateTimeoutMs_ = kStateTimeoutMs;
    int cooldownMs_ = kRestartCooldownMs;

    // Samples. `wantedSamples_` is the latest set asked for; `acceptedSamples_` is the last
    // one the host accepted. A refusal drops the wanted set back to the accepted one.
    std::vector<std::uint8_t> wantedSamples_;
    std::vector<std::uint8_t> acceptedSamples_;
    bool haveWantedSamples_ = false;   // an empty blob is never valid, but "nothing asked" is
    bool haveAcceptedSamples_ = false;
    bool wantedUnjudged_ = false;      // the wanted set has not been accepted or refused yet
    int wantedBroken_ = 0;             // exchanges of the wanted set that ended without an answer
    std::string samplesError_;
    bool samplesRefused_ = false;      // samplesError_ is a refusal, not a "no host answered"
    std::uint64_t samplesExchanges_ = 0;
};
