// 32-bit float WAV files (M5 recording).
//
// Float samples are written exactly, with no dither or rounding, so a take that is
// recorded and read back matches the render it came from bit for bit.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace phylo {

// Writes `interleaved` (frames * channels samples). Returns false on I/O failure.
bool writeWavFloat(const std::string& path, const std::vector<float>& interleaved, int channels,
                   int sampleRate);

// Reads a file written by writeWavFloat. Returns false for any other format.
bool readWavFloat(const std::string& path, std::vector<float>& interleaved, int& channels,
                  int& sampleRate);

// Collects a take block by block, then saves it as a float WAV.
class TakeRecorder
{
public:
    void begin(int channels, int sampleRate);
    void append(const float* interleaved, std::size_t frames);
    bool save(const std::string& path) const;
    const std::vector<float>& samples() const noexcept { return samples_; }

private:
    int channels_ = 2;
    int sampleRate_ = 48000;
    std::vector<float> samples_;
};

} // namespace phylo
