// Sums mono track blocks into a stereo master (M5).
//
// Each track has a gain, a pan and a mute. Pan uses an equal-power law: -1 is hard
// left, 0 is centre, +1 is hard right. The master output is stereo, interleaved (L R).
// There is no limiter: the sum can go above 1.0, and the caller decides what to do.
#pragma once

#include <cstddef>
#include <vector>

namespace phylo {

class Mixer
{
public:
    explicit Mixer(std::size_t tracks);

    std::size_t tracks() const noexcept { return strips_.size(); }

    // Linear gain, 0 or more. Negative values are treated as 0. Tracks are not changed
    // by an out-of-range index; the call is ignored.
    void setGain(std::size_t track, float linear);
    // Pan in [-1, 1]; values outside are clamped.
    void setPan(std::size_t track, float pan);
    void setMute(std::size_t track, bool muted);
    void setMasterGain(float linear);

    // trackBlocks[t] holds mono samples for track t. Missing tracks or samples count as
    // silence. Writes frames * 2 interleaved samples to `out`.
    void mix(const std::vector<std::vector<float>>& trackBlocks, std::size_t frames,
             std::vector<float>& out) const;

private:
    struct Strip
    {
        float gain = 1.0f;
        float pan = 0.0f;
        bool muted = false;
    };
    std::vector<Strip> strips_;
    float master_ = 1.0f;
};

// Decibels to linear gain. 0 dB is 1.0.
float dbToGain(float db) noexcept;

} // namespace phylo
