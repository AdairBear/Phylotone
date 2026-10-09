#include "phylo/Mixer.h"

#include <algorithm>
#include <cmath>

namespace phylo {

namespace {
constexpr float kPi = 3.14159265358979323846f;
}

Mixer::Mixer(std::size_t tracks) : strips_(tracks) {}

void Mixer::setGain(std::size_t track, float linear)
{
    if (track < strips_.size())
        strips_[track].gain = std::max(0.0f, linear);
}

void Mixer::setPan(std::size_t track, float pan)
{
    if (track < strips_.size())
        strips_[track].pan = std::clamp(pan, -1.0f, 1.0f);
}

void Mixer::setMute(std::size_t track, bool muted)
{
    if (track < strips_.size())
        strips_[track].muted = muted;
}

void Mixer::setMasterGain(float linear)
{
    master_ = std::max(0.0f, linear);
}

void Mixer::mix(const std::vector<std::vector<float>>& trackBlocks, std::size_t frames,
                std::vector<float>& out) const
{
    out.assign(frames * 2, 0.0f);

    for (std::size_t t = 0; t < strips_.size() && t < trackBlocks.size(); ++t)
    {
        const Strip& s = strips_[t];
        if (s.muted)
            continue;

        // Equal power: left^2 + right^2 == 1 for every pan position.
        const float angle = (s.pan + 1.0f) * kPi / 4.0f;
        const float g = s.gain * master_;
        const float left = g * std::cos(angle);
        const float right = g * std::sin(angle);

        const auto& block = trackBlocks[t];
        const std::size_t n = std::min(frames, block.size());
        for (std::size_t i = 0; i < n; ++i)
        {
            out[2 * i] += left * block[i];
            out[2 * i + 1] += right * block[i];
        }
    }
}

float dbToGain(float db) noexcept
{
    return std::pow(10.0f, db / 20.0f);
}

} // namespace phylo
