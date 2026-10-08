#include "PluginHostSupervisor.h"

#include <algorithm>
#include <thread>

using phylo::host::Frame;
using phylo::host::MsgType;

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
    retryAt_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(kRestartCooldownMs);
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
    if (!awaitFrame(MsgType::Hello, kReplyTimeoutMs, frame))
    {
        fail("host did not say hello");
        return false;
    }

    if (!pluginPath_.empty())
    {
        const auto load = phylo::host::encodeFrame(MsgType::Load, phylo::host::encodeText(pluginPath_));
        if (!pipe_.writeAll(load.data(), load.size()) || !awaitFrame(MsgType::Loaded, kReplyTimeoutMs, frame))
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
            retryAt_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(kRestartCooldownMs);
            pipe_.kill();
            return false;
        }
    }

    state_ = State::Running;
    error_.clear();
    return true;
}

void PluginHostSupervisor::render(const phylo::host::ProcessRequest& in, std::vector<float>& out)
{
    const std::size_t samples = static_cast<std::size_t>(in.frames) * in.channels;
    out.assign(samples, 0.0f);

    if (!ensureRunning())
        return;

    const auto request = phylo::host::encodeFrame(MsgType::Process, phylo::host::encodeProcess(in));
    if (!pipe_.writeAll(request.data(), request.size()))
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

void PluginHostSupervisor::killHostForTesting()
{
    // State stays Running: the crash is found by the next render, as in real life.
    pipe_.killChildForTesting();
}
