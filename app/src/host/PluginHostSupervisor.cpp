#include "PluginHostSupervisor.h"

#include "phylo/samples/SampleBlob.h"

#include <algorithm>
#include <thread>

using phylo::host::Frame;
using phylo::host::MsgType;

namespace
{
// Cuts a host's refusal text to the cap, never in the middle of a UTF-8 character.
std::string capReason(std::string text)
{
    const std::size_t cap = PluginHostSupervisor::kMaxReasonBytes;
    if (text.size() <= cap)
        return text;
    std::size_t cut = cap;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
        --cut; // text[cut] continues a character: move back to where that character starts
    text.resize(cut);
    text += "...";
    return text;
}
} // namespace

PluginHostSupervisor::PluginHostSupervisor(std::string hostExecutable, std::string pluginPath)
    : hostExecutable_(std::move(hostExecutable)), pluginPath_(std::move(pluginPath))
{
}

void PluginHostSupervisor::fail(const std::string& why)
{
    pipe_.kill();
    decoder_ = phylo::host::FrameDecoder{};
    state_ = State::Failed;
    error_ = why;
    retryAt_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(cooldownMs_);
}

bool PluginHostSupervisor::awaitFrame(MsgType type, int timeoutMs, Frame& out)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::uint8_t chunk[4096];
    for (;;)
    {
        while (decoder_.next(out))
        {
            if (out.type == type)
                return true;
            if (out.type == MsgType::Error)
            {
                std::string text;
                phylo::host::decodeText(out.payload, text);
                error_ = "host error: " + text;
                return false;
            }
            // Other frames (for example Hello while waiting for Audio) are skipped.
        }
        if (decoder_.failed())
        {
            error_ = decoder_.error();
            return false;
        }

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   deadline - std::chrono::steady_clock::now())
                                   .count();
        if (remaining <= 0)
            return false;

        const long n = pipe_.readWithTimeout(chunk, sizeof chunk, static_cast<int>(remaining));
        if (n < 0)
        {
            error_ = "host closed its output";
            return false;
        }
        if (n > 0)
            decoder_.feed(chunk, static_cast<std::size_t>(n));
    }
}

bool PluginHostSupervisor::ensureRunning()
{
    if (pipe_.running() && state_ == State::Running)
        return true;

    if (state_ == State::Failed && std::chrono::steady_clock::now() < retryAt_)
        return false;

    decoder_ = phylo::host::FrameDecoder{};
    if (!pipe_.start(hostExecutable_, {}))
    {
        fail("could not start " + hostExecutable_);
        return false;
    }

    Frame frame;
    if (!awaitFrame(MsgType::Hello, kStartTimeoutMs, frame))
    {
        fail("host did not say hello");
        return false;
    }

    if (!pluginPath_.empty())
    {
        const auto load = phylo::host::encodeFrame(MsgType::Load, phylo::host::encodeText(pluginPath_));
        if (!pipe_.writeAllWithTimeout(load.data(), load.size(), kStartTimeoutMs) ||
            !awaitFrame(MsgType::Loaded, kStartTimeoutMs, frame))
        {
            fail(error_.empty() ? "host did not confirm the plugin load" : error_);
            return false;
        }
        std::string loadError;
        phylo::host::decodeText(frame.payload, loadError);
        if (!loadError.empty())
        {
            // A plugin that will not load is not a crash. Keep the host up, and stay silent.
            error_ = loadError;
            state_ = State::Failed;
            retryAt_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(cooldownMs_);
            pipe_.kill();
            return false;
        }
    }

    state_ = State::Running;
    error_.clear();

    if (!restoredState_.empty())
    {
        stateAccepted_ = sendState(restoredState_);
        if (state_ != State::Running) // a broken host during the state: start again later
            return false;
    }

    // Samples after the plugin state, so the plugin has its own state before the slots.
    if (haveWantedSamples_ && !sendWantedSamples())
        return false;
    return true;
}

void PluginHostSupervisor::render(const phylo::host::ProcessRequest& in, std::vector<float>& out)
{
    const std::size_t samples = static_cast<std::size_t>(in.frames) * in.channels;
    out.assign(samples, 0.0f);

    if (!ensureRunning())
        return;

    const auto request = phylo::host::encodeFrame(MsgType::Process, phylo::host::encodeProcess(in));
    if (!pipe_.writeAllWithTimeout(request.data(), request.size(), kReplyTimeoutMs))
    {
        fail("host pipe broke while sending");
        return;
    }

    Frame frame;
    if (!awaitFrame(MsgType::Audio, kReplyTimeoutMs, frame))
    {
        fail(error_.empty() ? "host did not answer in time" : error_);
        return;
    }

    std::uint32_t frames = 0, channels = 0;
    std::vector<float> audio;
    if (!phylo::host::decodeAudio(frame.payload, frames, channels, audio) || frames != in.frames ||
        channels != in.channels)
    {
        fail("host answered with the wrong size");
        return;
    }
    out = std::move(audio);
}

bool PluginHostSupervisor::setState(const std::vector<std::uint8_t>& bytes)
{
    restoredState_ = bytes;
    stateAccepted_ = true;
    stateError_.clear();

    // A fresh start sends restoredState_ inside ensureRunning; a running host gets it here.
    const bool wasRunning = pipe_.running() && state_ == State::Running;
    if (!ensureRunning())
        return false;
    if (wasRunning)
        stateAccepted_ = sendState(bytes);
    if (!stateAccepted_ && !stateError_.empty())
        error_ = stateError_; // the samples sent after a fresh start would have cleared it
    return stateAccepted_;
}

bool PluginHostSupervisor::setSamples(const std::vector<std::uint8_t>& blob)
{
    samplesError_.clear();
    samplesRefused_ = false;

    // Bytes that are not a sample blob at all can never be an accepted set. The full check is
    // left to the host on purpose: a damaged blob must reach it and be refused there.
    if (!phylo::samples::looksLikeSampleBlob(blob.data(), blob.size()))
    {
        samplesError_ = "not a sample blob";
        samplesRefused_ = true;
        ++samplesExchanges_;
        return false;
    }

    wantedSamples_ = blob;
    haveWantedSamples_ = true;
    wantedUnjudged_ = true;
    wantedBroken_ = 0;

    // A fresh start sends wantedSamples_ inside ensureRunning; a running host gets it here.
    if (!(pipe_.running() && state_ == State::Running))
    {
        const std::uint64_t before = samplesExchanges_;
        if (!ensureRunning())
        {
            if (samplesExchanges_ == before)
            {
                // The host was down or in cooldown: the blob was never tried. It is kept.
                samplesError_ = error_;
                samplesRefused_ = false;
                ++samplesExchanges_;
            }
            return false;
        }
        return samplesError_.empty();
    }

    std::string text;
    switch (exchangeState(wantedSamples_, text, "samples"))
    {
    case StateReply::Accepted:
        acceptedSamples_ = wantedSamples_;
        haveAcceptedSamples_ = true;
        wantedUnjudged_ = false;
        ++samplesExchanges_;
        return true;
    case StateReply::Refused:
        // The host keeps what it had. Go back to the set that is in force.
        samplesError_ = text;
        samplesRefused_ = true;
        haveWantedSamples_ = haveAcceptedSamples_;
        wantedSamples_ = acceptedSamples_;
        wantedUnjudged_ = false;
        ++samplesExchanges_;
        return false;
    case StateReply::Broken:
        samplesExchangeBroken(text); // not judged: the wanted set stays for one more try
        return false;
    }
    return false;
}

void PluginHostSupervisor::samplesExchangeBroken(const std::string& text)
{
    ++samplesExchanges_;
    samplesError_ = text;
    samplesRefused_ = false;
    if (!wantedUnjudged_ || ++wantedBroken_ < 2)
        return; // the accepted set, or the first miss: try again on the next host

    // The second try with this blob also ended without an answer. Stop sending it. The host
    // has been marked failed, so the next start sends the accepted set.
    wantedUnjudged_ = false;
    wantedBroken_ = 0;
    wantedSamples_ = acceptedSamples_;
    haveWantedSamples_ = haveAcceptedSamples_;
    samplesError_ = haveAcceptedSamples_
                        ? "the host failed while loading the new samples; the previous slots were kept"
                        : "the host failed while loading the new samples; no samples are loaded";
    samplesRefused_ = true;
}

bool PluginHostSupervisor::sendWantedSamples()
{
    const bool wasUnjudged = wantedUnjudged_;
    std::string text;
    const StateReply reply = exchangeState(wantedSamples_, text, "samples");
    if (reply == StateReply::Broken)
    {
        samplesExchangeBroken(text);
        return false;
    }

    ++samplesExchanges_;
    wantedUnjudged_ = false;
    wantedBroken_ = 0;
    if (reply == StateReply::Accepted)
    {
        acceptedSamples_ = wantedSamples_;
        haveAcceptedSamples_ = true;
        if (wasUnjudged)
            samplesError_.clear(); // a new set went through; a re-sent set leaves the old reason
        return true;
    }

    // Refused. Keep the reason: it must outlive the re-send of the previous set below.
    samplesError_ = text;
    samplesRefused_ = true;
    if (!haveAcceptedSamples_)
    {
        // Nothing was ever accepted. Drop the bad blob so it is not sent on every restart.
        haveWantedSamples_ = false;
        wantedSamples_.clear();
        return true;
    }
    if (acceptedSamples_ != wantedSamples_)
    {
        wantedSamples_ = acceptedSamples_;
        std::string ignored;
        if (exchangeState(wantedSamples_, ignored, "samples") == StateReply::Broken)
        {
            ++samplesExchanges_;
            return false; // the refusal stays as the reason; the next start tries again
        }
    }
    return true;
}

PluginHostSupervisor::SamplesOutcome PluginHostSupervisor::samplesOutcome() const
{
    SamplesOutcome outcome;
    outcome.accepted = samplesError_.empty();
    outcome.refused = samplesRefused_;
    outcome.error = samplesError_;
    return outcome;
}

void PluginHostSupervisor::discardStaleFrames()
{
    std::uint8_t chunk[4096];
    for (int i = 0; i < 64; ++i) // bounded: a host that never stops writing cannot hold us here
    {
        const long n = pipe_.readWithTimeout(chunk, sizeof chunk, 0);
        if (n <= 0)
            break;
        decoder_.feed(chunk, static_cast<std::size_t>(n));
    }
    Frame stale;
    while (decoder_.next(stale))
    {
    }
}

PluginHostSupervisor::StateReply PluginHostSupervisor::exchangeState(const std::vector<std::uint8_t>& bytes,
                                                                     std::string& text, const char* what)
{
    discardStaleFrames();

    const auto request = phylo::host::encodeFrame(MsgType::SetState, bytes);
    if (!pipe_.writeAllWithTimeout(request.data(), request.size(), stateTimeoutMs_))
    {
        fail(std::string("host pipe broke while sending the ") + what);
        text = error_;
        return StateReply::Broken;
    }

    error_.clear(); // so a failure below reports its own cause, not an old one
    Frame frame;
    if (!awaitFrame(MsgType::StateResult, stateTimeoutMs_, frame))
    {
        fail(error_.empty() ? std::string("host did not answer the ") + what + " in time" : error_);
        text = error_;
        return StateReply::Broken;
    }

    text.clear();
    phylo::host::decodeText(frame.payload, text);
    text = capReason(std::move(text));
    return text.empty() ? StateReply::Accepted : StateReply::Refused;
}

bool PluginHostSupervisor::sendState(const std::vector<std::uint8_t>& bytes)
{
    std::string text;
    const StateReply reply = exchangeState(bytes, text, "state");
    if (reply == StateReply::Refused)
    {
        stateError_ = "state refused: " + text;
        error_ = stateError_;
    }
    return reply == StateReply::Accepted;
}

void PluginHostSupervisor::killHostForTesting()
{
    // State stays Running: the crash is found by the next render, as in real life.
    pipe_.killChildForTesting();
}
