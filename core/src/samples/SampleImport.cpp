#include "phylo/samples/SampleImport.h"

#include "phylo/samples/SampleBlob.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace phylo::samples
{

namespace
{

constexpr double kPi = 3.14159265358979323846;

// Resampler shape: zero crossings of the (scaled) sinc on each side of the centre, and how
// finely the kernel table is sampled between two zero crossings.
constexpr int kZeroCrossings = 32;
constexpr int kTableSteps = 2048;

// The largest legal file: 60 s of 192 kHz stereo 32-bit float, plus room for other chunks.
constexpr std::uint64_t kMaxFileBytes =
    static_cast<std::uint64_t>(kMaxSourceRateHz) * kMaxChannels * 4u * kMaxSampleSeconds + (1u << 20);

constexpr std::uint16_t kTagPcm = 1;
constexpr std::uint16_t kTagFloat = 3;
constexpr std::uint16_t kTagExtensible = 0xFFFE;

std::uint32_t get32(const std::uint8_t* p)
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint16_t get16(const std::uint8_t* p)
{
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

// Leaves `out` empty and returns the refusal.
ImportResult refuse(ImportError code, std::string message, ImportedSample& out)
{
    out.channels = 1;
    out.samples.clear();
    out.samples.shrink_to_fit();
    out.sourceSampleRate = 0;
    out.sourceBits = 0;
    out.sourceIsFloat = false;
    return {code, std::move(message)};
}

std::string describeFormat(std::uint16_t tag, std::uint16_t bits)
{
    switch (tag)
    {
        case 1: return std::to_string(bits) + "-bit PCM";
        case 3: return std::to_string(bits) + "-bit float";
        case 6: return "A-law";
        case 7: return "mu-law";
        default: return "format tag " + std::to_string(tag) + " (" + std::to_string(bits) + "-bit)";
    }
}

std::string seconds1(std::uint64_t frames, std::uint32_t rate)
{
    // One decimal, rounded to nearest, in integer arithmetic.
    const std::uint64_t tenths = (frames * 10u + rate / 2u) / rate;
    return std::to_string(tenths / 10u) + "." + std::to_string(tenths % 10u);
}

// Kernel g(u) = sinc(u) * window(u / kZeroCrossings) for u in [0, kZeroCrossings], sampled
// kTableSteps times per unit. The window is a 4-term Blackman-Harris.
const std::vector<double>& kernelTable()
{
    static const std::vector<double> table = []
    {
        std::vector<double> t(static_cast<std::size_t>(kZeroCrossings) * kTableSteps + 2, 0.0);
        for (std::size_t i = 0; i < t.size(); ++i)
        {
            const double u = static_cast<double>(i) / kTableSteps;
            const double x = u / kZeroCrossings;
            if (x >= 1.0)
                continue;
            const double sinc = u < 1e-12 ? 1.0 : std::sin(kPi * u) / (kPi * u);
            const double w = 0.35875 + 0.48829 * std::cos(kPi * x) + 0.14128 * std::cos(2.0 * kPi * x) +
                             0.01168 * std::cos(3.0 * kPi * x);
            t[i] = sinc * w;
        }
        return t;
    }();
    return table;
}

double kernelAt(const std::vector<double>& table, double u)
{
    // u >= 0 and below kZeroCrossings, so the index and the one after it are in the table.
    const double pos = u * kTableSteps;
    const std::size_t i = static_cast<std::size_t>(pos);
    const double frac = pos - static_cast<double>(i);
    return table[i] + (table[i + 1] - table[i]) * frac;
}

} // namespace

std::vector<float> resampleTo48k(const std::vector<float>& interleaved, int channels, int sourceRate)
{
    if (sourceRate == static_cast<int>(kSampleRateHz))
        return interleaved;
    if (channels < 1 || sourceRate < 1)
        return {};

    const std::size_t ch = static_cast<std::size_t>(channels);
    const std::uint64_t inFrames = interleaved.size() / ch;
    if (inFrames == 0)
        return {};

    const std::uint64_t rate = static_cast<std::uint64_t>(sourceRate);
    std::uint64_t outFrames = (inFrames * kSampleRateHz + rate / 2u) / rate;
    outFrames = std::min<std::uint64_t>(outFrames, kMaxSampleFrames);

    // Cutoff relative to the source Nyquist: 1 when upsampling, the output Nyquist when
    // downsampling, so nothing above 24 kHz survives to alias.
    const double cutoff =
        rate > kSampleRateHz ? static_cast<double>(kSampleRateHz) / static_cast<double>(rate) : 1.0;
    const std::int64_t reach = static_cast<std::int64_t>(std::ceil(kZeroCrossings / cutoff));

    const std::vector<double>& table = kernelTable();
    std::vector<float> out(static_cast<std::size_t>(outFrames) * ch);
    std::vector<double> weights(static_cast<std::size_t>(2 * reach + 1));
    std::vector<double> acc(ch);

    for (std::uint64_t i = 0; i < outFrames; ++i)
    {
        // Source position of output frame i is i * rate / 48000, split exactly into whole
        // frames and a fraction.
        const std::uint64_t num = i * rate;
        const std::int64_t centre = static_cast<std::int64_t>(num / kSampleRateHz);
        const double frac = static_cast<double>(num % kSampleRateHz) / static_cast<double>(kSampleRateHz);

        const std::int64_t first = std::max<std::int64_t>(centre - reach, 0);
        const std::int64_t last = std::min<std::int64_t>(centre + reach, static_cast<std::int64_t>(inFrames) - 1);

        double sum = 0.0;
        for (std::int64_t n = first; n <= last; ++n)
        {
            const double d = static_cast<double>(n - centre) - frac;
            const double u = std::fabs(d) * cutoff;
            const double w = u < kZeroCrossings ? kernelAt(table, u) : 0.0;
            weights[static_cast<std::size_t>(n - first)] = w;
            sum += w;
        }
        // Normalising by the weight sum keeps DC gain at exactly 1, edges included.
        const double scale = std::fabs(sum) > 1e-12 ? 1.0 / sum : 0.0;

        std::fill(acc.begin(), acc.end(), 0.0);
        for (std::int64_t n = first; n <= last; ++n)
        {
            const double w = weights[static_cast<std::size_t>(n - first)];
            const float* src = interleaved.data() + static_cast<std::size_t>(n) * ch;
            for (std::size_t c = 0; c < ch; ++c)
                acc[c] += w * static_cast<double>(src[c]);
        }
        for (std::size_t c = 0; c < ch; ++c)
            out[static_cast<std::size_t>(i) * ch + c] = static_cast<float>(acc[c] * scale);
    }
    return out;
}

ImportResult importWavBytes(const std::uint8_t* data, std::size_t size, ImportedSample& out)
{
    refuse(ImportError::None, {}, out); // start from an empty result

    if (data == nullptr || size < 12 || std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0)
        return refuse(ImportError::NotWav, "not a WAV file", out);

    bool haveFmt = false;
    std::uint16_t tag = 0, channels = 0, blockAlign = 0, bits = 0;
    std::uint32_t rate = 0;

    std::size_t pos = 12;
    while (size - pos >= 8) // pos never exceeds size
    {
        const std::uint8_t* head = data + pos;
        const std::uint64_t chunkSize = get32(head + 4);
        const std::size_t body = pos + 8;
        if (chunkSize > size - body)
            return refuse(ImportError::Damaged, "damaged WAV file (a chunk runs past the end)", out);
        const std::size_t len = static_cast<std::size_t>(chunkSize);

        if (std::memcmp(head, "fmt ", 4) == 0)
        {
            if (len < 16)
                return refuse(ImportError::Damaged, "damaged WAV file (format chunk too short)", out);
            const std::uint8_t* f = data + body;
            tag = get16(f);
            channels = get16(f + 2);
            rate = get32(f + 4);
            blockAlign = get16(f + 12);
            bits = get16(f + 14);
            if (tag == kTagExtensible)
            {
                if (len < 40)
                    return refuse(ImportError::Damaged, "damaged WAV file (extensible format chunk too short)", out);
                tag = get16(f + 24); // first two bytes of the sub-format GUID
            }
            haveFmt = true;

            const bool supported = (tag == kTagPcm && (bits == 16 || bits == 24)) || (tag == kTagFloat && bits == 32);
            if (!supported)
                return refuse(ImportError::UnsupportedFormat,
                              describeFormat(tag, bits) + " is not supported (16-bit, 24-bit or 32-bit float)", out);
            if (channels != 1 && channels != 2)
                return refuse(ImportError::UnsupportedChannels,
                              std::to_string(channels) + " channels is not supported (mono or stereo)", out);
            if (rate < static_cast<std::uint32_t>(kMinSourceRateHz) ||
                rate > static_cast<std::uint32_t>(kMaxSourceRateHz))
                return refuse(ImportError::UnsupportedRate,
                              std::to_string(rate) + " Hz is not supported (" + std::to_string(kMinSourceRateHz) +
                                  " to " + std::to_string(kMaxSourceRateHz) + " Hz)",
                              out);
            if (blockAlign != channels * (bits / 8))
                return refuse(ImportError::Damaged, "damaged WAV file (block size does not match the format)", out);
        }
        else if (std::memcmp(head, "data", 4) == 0)
        {
            if (!haveFmt)
                return refuse(ImportError::Damaged, "damaged WAV file (audio data before the format chunk)", out);

            if (len % blockAlign != 0)
                return refuse(ImportError::Damaged, "damaged WAV file (data is not a whole number of frames)", out);
            const std::uint64_t frames = len / blockAlign;
            if (frames == 0)
                return refuse(ImportError::Empty, "the file has no audio", out);

            // The length rule, from the header fields alone, before any sample is decoded.
            if (frames > static_cast<std::uint64_t>(kMaxSampleSeconds) * rate)
                return refuse(ImportError::TooLong,
                              "longer than " + std::to_string(kMaxSampleSeconds) + " s (" + seconds1(frames, rate) +
                                  " s)",
                              out);

            const std::size_t count = static_cast<std::size_t>(frames) * channels;
            std::vector<float> pcm(count);
            const std::uint8_t* p = data + body;
            if (tag == kTagFloat)
            {
                for (std::size_t i = 0; i < count; ++i)
                {
                    const std::uint32_t raw = get32(p + i * 4);
                    if ((raw & 0x7F800000u) == 0x7F800000u)
                        return refuse(ImportError::NotFinite, "the file contains NaN or infinite samples", out);
                    std::memcpy(&pcm[i], &raw, sizeof(float));
                }
            }
            else if (bits == 16)
            {
                for (std::size_t i = 0; i < count; ++i)
                    pcm[i] = static_cast<float>(static_cast<std::int16_t>(get16(p + i * 2))) / 32768.0f;
            }
            else
            {
                for (std::size_t i = 0; i < count; ++i)
                {
                    const std::uint8_t* s = p + i * 3;
                    std::uint32_t v = static_cast<std::uint32_t>(s[0]) | (static_cast<std::uint32_t>(s[1]) << 8) |
                                      (static_cast<std::uint32_t>(s[2]) << 16);
                    if (v & 0x800000u)
                        v |= 0xFF000000u; // sign-extend
                    pcm[i] = static_cast<float>(static_cast<std::int32_t>(v)) / 8388608.0f;
                }
            }

            std::vector<float> converted = resampleTo48k(pcm, channels, static_cast<int>(rate));
            const std::size_t maxSamples = static_cast<std::size_t>(kMaxSampleFrames) * channels;
            if (converted.size() > maxSamples)
                converted.resize(maxSamples);

            out.channels = channels;
            out.samples = std::move(converted);
            out.sourceSampleRate = static_cast<int>(rate);
            out.sourceBits = bits;
            out.sourceIsFloat = (tag == kTagFloat);
            return {};
        }

        // Chunks are word-aligned; the pad byte may be missing at the very end of the file.
        const std::size_t next = body + len + (len & 1u);
        if (next > size)
            break;
        pos = next;
    }

    if (!haveFmt)
        return refuse(ImportError::Damaged, "damaged WAV file (no format chunk)", out);
    return refuse(ImportError::Damaged, "damaged WAV file (no audio data)", out);
}

ImportResult importWavFile(const std::string& path, ImportedSample& out)
{
    refuse(ImportError::None, {}, out);

    std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
    if (!in)
        return refuse(ImportError::CannotOpen, "the file could not be opened", out);

    in.seekg(0, std::ios::end);
    const std::streamoff end = in.tellg();
    if (!in || end < 0)
        return refuse(ImportError::CannotOpen, "the file could not be read", out);
    if (static_cast<std::uint64_t>(end) > kMaxFileBytes)
        return refuse(ImportError::TooLong,
                      "file is too large to be a " + std::to_string(kMaxSampleSeconds) + " s sample", out);
    in.seekg(0, std::ios::beg);

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    if (!bytes.empty())
    {
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (static_cast<std::size_t>(in.gcount()) != bytes.size())
            return refuse(ImportError::CannotOpen, "the file could not be read", out);
    }
    return importWavBytes(bytes.data(), bytes.size(), out);
}

} // namespace phylo::samples
