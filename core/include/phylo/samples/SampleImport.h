// WAV import for the sample library (docs/SAMPLE_INTERFACE.md). JUCE-free.
//
// Reads a WAV file that is 16-bit PCM, 24-bit PCM or 32-bit float, mono or stereo, at any
// sample rate from kMinSourceRateHz to kMaxSourceRateHz, and converts it to the one format
// the plugin receives: 48 kHz, 32-bit float, interleaved, the same channel count.
//
// A file longer than 60 s is refused here, in the app, before anything is sent. The length
// is read from the file's header, so an over-long file is refused before its samples are
// decoded or resampled.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace phylo::samples
{

constexpr int kMinSourceRateHz = 8000;
constexpr int kMaxSourceRateHz = 192000;

enum class ImportError
{
    None,
    CannotOpen,          // the file could not be read
    NotWav,              // no RIFF/WAVE header
    Damaged,             // a chunk runs past the end of the file, no fmt or no data chunk,
                         // or the data size is not a whole number of frames
    UnsupportedFormat,   // not 16-bit PCM, 24-bit PCM or 32-bit float
    UnsupportedChannels, // not mono or stereo
    UnsupportedRate,     // sample rate outside kMinSourceRateHz to kMaxSourceRateHz
    Empty,               // no frames
    TooLong,             // longer than 60 s
    NotFinite            // a float sample is NaN or infinite
};

// `message` is the reason for the status line, for example
// "longer than 60 s (61.00 s)" or "8-bit PCM is not supported (16-bit, 24-bit or 32-bit float)".
struct ImportResult
{
    ImportError code = ImportError::None;
    std::string message;

    bool ok() const noexcept { return code == ImportError::None; }
};

struct ImportedSample
{
    int channels = 1;           // 1 or 2
    std::vector<float> samples; // interleaved, 48 kHz, frames * channels

    // What the source file was, for display.
    int sourceSampleRate = 0;
    int sourceBits = 0;
    bool sourceIsFloat = false;

    std::size_t frames() const noexcept
    {
        return channels > 0 ? samples.size() / static_cast<std::size_t>(channels) : 0;
    }
};

// Imports from WAV bytes already in memory. On a refusal `out` is left empty.
ImportResult importWavBytes(const std::uint8_t* data, std::size_t size, ImportedSample& out);

// Reads the file, then importWavBytes. The source file is only read, never changed. A file
// too large to be a 60 s sample in any supported format is refused before it is loaded.
ImportResult importWavFile(const std::string& path, ImportedSample& out);

// Converts interleaved audio to 48 kHz with a band-limited (Kaiser-windowed sinc) resampler.
// At 48 kHz the input is returned unchanged, bit for bit. The output has
// round(frames * 48000 / sourceRate) frames, but at least one frame for a non-empty input
// and at most 60 s (2 880 000 frames). Deterministic: the same input gives the same output
// on every run.
//
// The low-pass filter's transition band ends at the lower of the source Nyquist and 24 kHz,
// and it is flat to within 0.1 dB up to about 20 kHz (for 44.1 kHz and 96 kHz sources).
// Content from that Nyquist up is attenuated by about 80 dB, so it aliases (downsampling) or
// images (upsampling) only at about -80 dB. Content between roughly 20 kHz and that
// Nyquist is attenuated, not preserved.
std::vector<float> resampleTo48k(const std::vector<float>& interleaved, int channels, int sourceRate);

} // namespace phylo::samples
