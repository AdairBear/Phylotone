#include "phylo/samples/SampleImport.h"

#include "phylo/samples/SampleBlob.h"

#include "check.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

using namespace phylo::samples;

namespace
{

constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------------------
// Building WAV files in memory.

struct WavSpec
{
    std::uint16_t tag = 1;
    std::uint16_t bits = 16;
    std::uint16_t channels = 1;
    std::uint32_t rate = 48000;
    bool extensible = false;
    int blockAlign = -1;         // -1: the correct value
    bool listBeforeData = false; // an unknown chunk with an even size
    bool oddChunkBeforeData = false; // an unknown chunk with an odd size and a pad byte
    bool dataBeforeFmt = false;
    bool omitFmt = false;
    bool omitData = false;
    long long declaredDataSize = -1; // -1: the real size
    std::vector<std::uint8_t> data;
};

void put16(std::vector<std::uint8_t>& b, std::uint32_t v)
{
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
}

void put32(std::vector<std::uint8_t>& b, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

void putTag(std::vector<std::uint8_t>& b, const char* tag)
{
    b.insert(b.end(), tag, tag + 4);
}

std::vector<std::uint8_t> buildWav(const WavSpec& s)
{
    std::vector<std::uint8_t> fmt;
    const int align = s.blockAlign >= 0 ? s.blockAlign : s.channels * (s.bits / 8);
    put16(fmt, s.extensible ? 0xFFFE : s.tag);
    put16(fmt, s.channels);
    put32(fmt, s.rate);
    put32(fmt, s.rate * static_cast<std::uint32_t>(align));
    put16(fmt, static_cast<std::uint32_t>(align));
    put16(fmt, s.bits);
    if (s.extensible)
    {
        put16(fmt, 22);     // cbSize
        put16(fmt, s.bits); // valid bits
        put32(fmt, 3);      // channel mask
        put16(fmt, s.tag);  // sub-format GUID: tag, then the fixed tail
        const std::uint8_t tail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
        fmt.insert(fmt.end(), tail, tail + 14);
    }

    auto fmtChunk = [&](std::vector<std::uint8_t>& b)
    {
        putTag(b, "fmt ");
        put32(b, static_cast<std::uint32_t>(fmt.size()));
        b.insert(b.end(), fmt.begin(), fmt.end());
    };
    auto dataChunk = [&](std::vector<std::uint8_t>& b)
    {
        putTag(b, "data");
        put32(b, s.declaredDataSize >= 0 ? static_cast<std::uint32_t>(s.declaredDataSize)
                                         : static_cast<std::uint32_t>(s.data.size()));
        b.insert(b.end(), s.data.begin(), s.data.end());
        if (s.data.size() & 1u)
            b.push_back(0);
    };

    std::vector<std::uint8_t> body;
    putTag(body, "WAVE");
    if (s.dataBeforeFmt && !s.omitData)
        dataChunk(body);
    if (!s.omitFmt)
        fmtChunk(body);
    if (s.listBeforeData)
    {
        putTag(body, "LIST");
        put32(body, 8);
        putTag(body, "INFO");
        put32(body, 0);
    }
    if (s.oddChunkBeforeData)
    {
        putTag(body, "junk");
        put32(body, 3);
        body.push_back(1);
        body.push_back(2);
        body.push_back(3);
        body.push_back(0); // pad
    }
    if (!s.dataBeforeFmt && !s.omitData)
        dataChunk(body);

    std::vector<std::uint8_t> file;
    putTag(file, "RIFF");
    put32(file, static_cast<std::uint32_t>(body.size()));
    file.insert(file.end(), body.begin(), body.end());
    return file;
}

std::vector<std::uint8_t> bytes16(const std::vector<int>& v)
{
    std::vector<std::uint8_t> b;
    for (int x : v)
        put16(b, static_cast<std::uint32_t>(x) & 0xFFFFu);
    return b;
}

std::vector<std::uint8_t> bytes24(const std::vector<int>& v)
{
    std::vector<std::uint8_t> b;
    for (int x : v)
    {
        const std::uint32_t u = static_cast<std::uint32_t>(x);
        b.push_back(static_cast<std::uint8_t>(u));
        b.push_back(static_cast<std::uint8_t>(u >> 8));
        b.push_back(static_cast<std::uint8_t>(u >> 16));
    }
    return b;
}

std::vector<std::uint8_t> bytesFloat(const std::vector<float>& v)
{
    std::vector<std::uint8_t> b;
    for (float x : v)
    {
        std::uint32_t u;
        std::memcpy(&u, &x, 4);
        put32(b, u);
    }
    return b;
}

std::vector<std::uint8_t> bytesBits(std::uint32_t bits)
{
    std::vector<std::uint8_t> b;
    put32(b, bits);
    return b;
}

ImportResult run(const std::vector<std::uint8_t>& wav, ImportedSample& out)
{
    return importWavBytes(wav.data(), wav.size(), out);
}

WavSpec pcm16(std::uint16_t ch, std::uint32_t rate, const std::vector<int>& v)
{
    WavSpec s;
    s.channels = ch;
    s.rate = rate;
    s.data = bytes16(v);
    return s;
}

// Expects a refusal with `code`, and an empty `out`.
void expectRefusal(const std::vector<std::uint8_t>& wav, ImportError code)
{
    ImportedSample out;
    out.samples.assign(10, 1.0f); // must be cleared on refusal
    const ImportResult r = run(wav, out);
    CHECK(r.code == code);
    CHECK(!r.ok());
    CHECK(!r.message.empty());
    CHECK(out.samples.empty());
}

// ---------------------------------------------------------------------------------------
// Signals.

std::vector<float> sine(int frames, double freq, double rate, double amp, int channels = 1, int sineChannel = 0)
{
    std::vector<float> v(static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels), 0.0f);
    for (int i = 0; i < frames; ++i)
        v[static_cast<std::size_t>(i) * static_cast<std::size_t>(channels) + static_cast<std::size_t>(sineChannel)] =
            static_cast<float>(amp * std::sin(2.0 * kPi * freq * i / rate));
    return v;
}

long long expectedFrames(long long frames, long long rate)
{
    return (frames * 48000 + rate / 2) / rate;
}

double maxSineError(const std::vector<float>& got, double freq, double amp, std::size_t margin)
{
    double worst = 0.0;
    for (std::size_t i = margin; i + margin < got.size(); ++i)
    {
        const double ideal = amp * std::sin(2.0 * kPi * freq * static_cast<double>(i) / 48000.0);
        worst = std::fmax(worst, std::fabs(static_cast<double>(got[i]) - ideal));
    }
    return worst;
}

// Level of the tone at `freq` in the middle of `x` (the first and last tenth are ignored), as
// the amplitude of a sine, found with a single-bin DFT.
double toneAmplitude(const std::vector<float>& x, double freq, double rate)
{
    const std::size_t a = x.size() / 10;
    const std::size_t b = x.size() - a;
    double re = 0.0, im = 0.0;
    for (std::size_t i = a; i < b; ++i)
    {
        const double ph = 2.0 * kPi * freq * static_cast<double>(i) / rate;
        re += static_cast<double>(x[i]) * std::cos(ph);
        im += static_cast<double>(x[i]) * std::sin(ph);
    }
    return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(b - a);
}

double toDb(double ratio)
{
    return 20.0 * std::log10(ratio + 1e-30);
}

// 16-bit mono silence of `frames` frames.
WavSpec silence16(std::uint32_t rate, std::uint64_t frames)
{
    WavSpec s;
    s.rate = rate;
    s.data.assign(static_cast<std::size_t>(frames) * 2u, 0);
    return s;
}

} // namespace

// ---------------------------------------------------------------------------------------
// Decoding at 48 kHz.

TEST(import_pcm16_mono)
{
    ImportedSample out;
    const ImportResult r = run(buildWav(pcm16(1, 48000, {0x4000, -32768, 0, 32767, -16384})), out);
    CHECK(r.ok());
    CHECK(r.code == ImportError::None);
    CHECK_EQ(out.channels, 1);
    CHECK_EQ(out.frames(), 5u);
    CHECK_EQ(out.sourceSampleRate, 48000);
    CHECK_EQ(out.sourceBits, 16);
    CHECK(!out.sourceIsFloat);
    CHECK(out.samples[0] == 0.5f);
    CHECK(out.samples[1] == -1.0f);
    CHECK(out.samples[2] == 0.0f);
    CHECK(out.samples[3] == 32767.0f / 32768.0f);
    CHECK(out.samples[4] == -0.5f);
}

TEST(import_pcm16_stereo)
{
    ImportedSample out;
    const ImportResult r = run(buildWav(pcm16(2, 48000, {0x4000, -0x4000, -32768, 0x2000})), out);
    CHECK(r.ok());
    CHECK_EQ(out.channels, 2);
    CHECK_EQ(out.frames(), 2u);
    CHECK(out.samples[0] == 0.5f);
    CHECK(out.samples[1] == -0.5f);
    CHECK(out.samples[2] == -1.0f);
    CHECK(out.samples[3] == 0.25f);
}

TEST(import_pcm24_mono_and_stereo)
{
    WavSpec s;
    s.bits = 24;
    s.data = bytes24({0x400000, -0x800000, 0x7FFFFF, 0, -1});
    ImportedSample out;
    CHECK(run(buildWav(s), out).ok());
    CHECK_EQ(out.channels, 1);
    CHECK_EQ(out.frames(), 5u);
    CHECK_EQ(out.sourceBits, 24);
    CHECK(!out.sourceIsFloat);
    CHECK(out.samples[0] == 0.5f);
    CHECK(out.samples[1] == -1.0f);
    CHECK(out.samples[2] == 8388607.0f / 8388608.0f);
    CHECK(out.samples[3] == 0.0f);
    CHECK(out.samples[4] == -1.0f / 8388608.0f);

    s.channels = 2;
    s.data = bytes24({0x400000, -0x400000, -0x800000, 0x200000});
    CHECK(run(buildWav(s), out).ok());
    CHECK_EQ(out.channels, 2);
    CHECK_EQ(out.frames(), 2u);
    CHECK(out.samples[0] == 0.5f);
    CHECK(out.samples[1] == -0.5f);
    CHECK(out.samples[2] == -1.0f);
    CHECK(out.samples[3] == 0.25f);
}

TEST(import_float32_bit_exact)
{
    WavSpec s;
    s.tag = 3;
    s.bits = 32;
    const std::vector<float> in = {0.1f, -0.3f, 2.5f, -7.0f, 1.0e-30f, 0.0f, -0.0f, 3.0e38f};
    s.data = bytesFloat(in);
    ImportedSample out;
    CHECK(run(buildWav(s), out).ok());
    CHECK_EQ(out.channels, 1);
    CHECK_EQ(out.frames(), in.size());
    CHECK_EQ(out.sourceBits, 32);
    CHECK(out.sourceIsFloat);
    CHECK(out.samples.size() == in.size() &&
          std::memcmp(out.samples.data(), in.data(), in.size() * sizeof(float)) == 0);

    s.channels = 2;
    CHECK(run(buildWav(s), out).ok());
    CHECK_EQ(out.channels, 2);
    CHECK_EQ(out.frames(), in.size() / 2);
    CHECK(out.samples.size() == in.size() &&
          std::memcmp(out.samples.data(), in.data(), in.size() * sizeof(float)) == 0);
}

TEST(import_extensible_pcm_and_float)
{
    WavSpec s;
    s.extensible = true;
    s.tag = 1;
    s.bits = 16;
    s.data = bytes16({0x4000, -32768});
    ImportedSample out;
    CHECK(run(buildWav(s), out).ok());
    CHECK(out.samples.size() == 2 && out.samples[0] == 0.5f && out.samples[1] == -1.0f);
    CHECK_EQ(out.sourceBits, 16);
    CHECK(!out.sourceIsFloat);

    s.bits = 24;
    s.data = bytes24({0x400000, -0x800000});
    CHECK(run(buildWav(s), out).ok());
    CHECK(out.samples.size() == 2 && out.samples[0] == 0.5f && out.samples[1] == -1.0f);

    s.tag = 3;
    s.bits = 32;
    s.data = bytesFloat({1.5f, -0.25f});
    CHECK(run(buildWav(s), out).ok());
    CHECK(out.samples.size() == 2 && out.samples[0] == 1.5f && out.samples[1] == -0.25f);
    CHECK(out.sourceIsFloat);

    // Extensible with an unsupported sub-format is still refused.
    s.tag = 6;
    s.bits = 16;
    s.data = bytes16({1, 2});
    expectRefusal(buildWav(s), ImportError::UnsupportedFormat);
}

TEST(import_extensible_short_fmt_is_damaged)
{
    WavSpec s;
    s.extensible = true;
    s.data = bytes16({1, 2});
    std::vector<std::uint8_t> wav = buildWav(s);
    // Shrink the declared fmt size to 18 (still inside the file); the chunk walk then
    // lands in the middle of the old fmt body, which is not a valid chunk.
    wav[16] = 18;
    ImportedSample out;
    CHECK(!run(wav, out).ok());
    CHECK(out.samples.empty());
}

TEST(import_skips_other_chunks)
{
    WavSpec s = pcm16(1, 48000, {0x4000, -32768, 0x2000});
    s.listBeforeData = true;
    s.oddChunkBeforeData = true;
    ImportedSample out;
    CHECK(run(buildWav(s), out).ok());
    CHECK_EQ(out.frames(), 3u);
    CHECK(out.samples[0] == 0.5f && out.samples[1] == -1.0f && out.samples[2] == 0.25f);
}

TEST(import_trailing_chunks_after_data_ignored)
{
    std::vector<std::uint8_t> wav = buildWav(pcm16(1, 48000, {0x4000, 0x2000}));
    putTag(wav, "LIST");
    put32(wav, 1000); // runs past the end, but we never get there
    ImportedSample out;
    CHECK(run(wav, out).ok());
    CHECK_EQ(out.frames(), 2u);
}

// ---------------------------------------------------------------------------------------
// Resampling.

TEST(resample_48k_is_identical)
{
    const std::vector<float> in = sine(1000, 440.0, 48000.0, 0.7, 2);
    const std::vector<float> out = resampleTo48k(in, 2, 48000);
    CHECK(out == in);
    CHECK(out.size() == in.size() && std::memcmp(out.data(), in.data(), in.size() * sizeof(float)) == 0);
}

TEST(resample_frame_counts)
{
    const int rates[] = {44100, 96000, 22050, 8000, 192000, 11025, 32000};
    for (int rate : rates)
    {
        const int frames = rate / 20 + 7;
        const std::vector<float> out = resampleTo48k(std::vector<float>(static_cast<std::size_t>(frames), 0.25f), 1, rate);
        CHECK_EQ(static_cast<long long>(out.size()), expectedFrames(frames, rate));
    }
    // Stereo counts frames, not samples.
    const std::vector<float> st = resampleTo48k(std::vector<float>(2 * 441, 0.1f), 2, 44100);
    CHECK_EQ(static_cast<long long>(st.size()), 2 * expectedFrames(441, 44100));
}

TEST(resample_sine_keeps_frequency_and_amplitude)
{
    const int rates[] = {44100, 96000, 22050};
    for (int rate : rates)
    {
        const int frames = rate / 5; // 0.2 s
        const std::vector<float> in = sine(frames, 1000.0, rate, 0.8);
        const std::vector<float> out = resampleTo48k(in, 1, rate);
        CHECK_EQ(static_cast<long long>(out.size()), expectedFrames(frames, rate));
        const double err = maxSineError(out, 1000.0, 0.8, 300);
        if (err > 1e-3)
            std::printf("  sine error at %d Hz: %g\n", rate, err);
        CHECK(err < 1e-3);
    }
}

TEST(resample_dc_holds)
{
    const int rates[] = {44100, 96000, 22050, 8000, 192000};
    for (int rate : rates)
    {
        const std::vector<float> in(static_cast<std::size_t>(rate / 10), 0.5f);
        const std::vector<float> out = resampleTo48k(in, 1, rate);
        double worstMiddle = 0.0, worstAll = 0.0;
        for (std::size_t i = 0; i < out.size(); ++i)
        {
            const double e = std::fabs(static_cast<double>(out[i]) - 0.5);
            worstAll = std::fmax(worstAll, e);
            if (i > 300 && i + 300 < out.size())
                worstMiddle = std::fmax(worstMiddle, e);
        }
        CHECK(worstMiddle < 1e-4);
        CHECK(worstAll < 1e-4); // edges are renormalised, so they do not dip either
    }
}

TEST(resample_stereo_channels_independent)
{
    const int rate = 44100;
    const int frames = 4410;
    const std::vector<float> in = sine(frames, 1000.0, rate, 0.8, 2, 0);
    const std::vector<float> out = resampleTo48k(in, 2, rate);
    CHECK_EQ(static_cast<long long>(out.size()), 2 * expectedFrames(frames, rate));
    std::vector<float> left, right;
    for (std::size_t i = 0; i + 1 < out.size(); i += 2)
    {
        left.push_back(out[i]);
        right.push_back(out[i + 1]);
    }
    CHECK(maxSineError(left, 1000.0, 0.8, 300) < 1e-3);
    double worstRight = 0.0;
    for (float x : right)
        worstRight = std::fmax(worstRight, std::fabs(static_cast<double>(x)));
    CHECK(worstRight < 1e-6);

    // The same mono signal resampled alone matches the left channel exactly.
    const std::vector<float> mono = resampleTo48k(sine(frames, 1000.0, rate, 0.8), 1, rate);
    CHECK(mono == left);
}

TEST(resample_downsampling_removes_tone_above_nyquist)
{
    const int rate = 96000;
    const int frames = 9600;
    const std::vector<float> in = sine(frames, 30000.0, rate, 1.0);
    const std::vector<float> out = resampleTo48k(in, 1, rate);
    double sumSq = 0.0;
    std::size_t n = 0;
    for (std::size_t i = 300; i + 300 < out.size(); ++i, ++n)
        sumSq += static_cast<double>(out[i]) * static_cast<double>(out[i]);
    const double rms = std::sqrt(sumSq / static_cast<double>(n));
    const double inRms = 1.0 / std::sqrt(2.0);
    CHECK(rms < 0.01 * inRms);
}

TEST(resample_stopband_96k_tone_above_nyquist)
{
    // 25 kHz at full scale would alias to 23 kHz; it must be at least 60 dB down (the filter
    // gives about 90).
    const int rate = 96000;
    const std::vector<float> in = sine(rate / 4, 25000.0, rate, 1.0);
    const std::vector<float> out = resampleTo48k(in, 1, rate);
    double sumSq = 0.0;
    std::size_t n = 0;
    for (std::size_t i = 300; i + 300 < out.size(); ++i, ++n)
        sumSq += static_cast<double>(out[i]) * static_cast<double>(out[i]);
    const double rms = std::sqrt(sumSq / static_cast<double>(n));
    CHECK(toDb(rms / (1.0 / std::sqrt(2.0))) < -60.0);
    CHECK(toDb(toneAmplitude(out, 23000.0, 48000.0)) < -60.0);

    // Just above 24 kHz too.
    const std::vector<float> near = resampleTo48k(sine(rate / 4, 24500.0, rate, 1.0), 1, rate);
    CHECK(toDb(toneAmplitude(near, 23500.0, 48000.0)) < -60.0);
}

TEST(resample_44k_tone_near_nyquist_leaves_no_image)
{
    // A 21.5 kHz tone at 44.1 kHz images at 44100 - 21500 = 22600 Hz once the rate goes up.
    const int rate = 44100;
    const std::vector<float> out = resampleTo48k(sine(rate / 4, 21500.0, rate, 1.0), 1, rate);
    CHECK(toDb(toneAmplitude(out, 22600.0, 48000.0)) < -60.0);
    // And at 22.0 kHz (image at 22.1 kHz), the closest a tone can get to the Nyquist.
    const std::vector<float> edge = resampleTo48k(sine(rate / 4, 22000.0, rate, 1.0), 1, rate);
    CHECK(toDb(toneAmplitude(edge, 22100.0, 48000.0)) < -60.0);
}

TEST(resample_passband_is_flat)
{
    const int rates[] = {44100, 96000};
    const double freqs[] = {1000.0, 18000.0, 20000.0};
    for (int rate : rates)
    {
        for (double f : freqs)
        {
            const std::vector<float> out = resampleTo48k(sine(rate / 4, f, rate, 0.8), 1, rate);
            const double db = toDb(toneAmplitude(out, f, 48000.0) / 0.8);
            if (std::fabs(db) >= 0.1)
                std::printf("  %d Hz source, %g Hz tone: %g dB\n", rate, f, db);
            CHECK(std::fabs(db) < 0.1);
        }
    }
}

TEST(resample_one_frame_is_never_emptied)
{
    const int rates[] = {192000, 176400, 96001, 96000, 88200, 64000};
    for (int rate : rates)
    {
        ImportedSample out;
        const ImportResult r = run(buildWav(pcm16(1, static_cast<std::uint32_t>(rate), {16384})), out);
        CHECK(r.ok());
        CHECK_EQ(out.frames(), 1u);
        if (out.frames() == 1)
            CHECK(std::fabs(out.samples[0] - 0.5f) < 1e-4f); // a single value is held
    }
    CHECK_EQ(resampleTo48k(std::vector<float>{0.5f, -0.5f}, 2, 192000).size(), 2u); // one stereo frame
}

TEST(resample_deterministic)
{
    const std::vector<float> in = sine(4410, 1234.5, 44100.0, 0.6, 2);
    const std::vector<float> a = resampleTo48k(in, 2, 44100);
    const std::vector<float> b = resampleTo48k(in, 2, 44100);
    CHECK(a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}

TEST(resample_via_import_reports_source)
{
    WavSpec s;
    s.rate = 44100;
    std::vector<int> v;
    for (int i = 0; i < 4410; ++i)
        v.push_back(static_cast<int>(16000.0 * std::sin(2.0 * kPi * 1000.0 * i / 44100.0)));
    s.data = bytes16(v);
    ImportedSample out;
    CHECK(run(buildWav(s), out).ok());
    CHECK_EQ(out.sourceSampleRate, 44100);
    CHECK_EQ(out.sourceBits, 16);
    CHECK_EQ(static_cast<long long>(out.frames()), expectedFrames(4410, 44100));
    CHECK(maxSineError(out.samples, 1000.0, 16000.0 / 32768.0, 300) < 1e-3);
}

// ---------------------------------------------------------------------------------------
// The 60 s rule.

TEST(length_exactly_60s_at_48k_accepted)
{
    WavSpec s;
    s.data.assign(2880000u * 2u, 0);
    ImportedSample out;
    CHECK(run(buildWav(s), out).ok());
    CHECK_EQ(out.frames(), 2880000u);
    CHECK_EQ(out.frames(), static_cast<std::size_t>(phylo::samples::kMaxSampleFrames));
}

TEST(length_60s_plus_one_frame_too_long)
{
    WavSpec s;
    s.data.assign(2880001u * 2u, 0);
    expectRefusal(buildWav(s), ImportError::TooLong);
}

TEST(length_61s_at_44k_too_long)
{
    WavSpec s;
    s.rate = 44100;
    s.data.assign(44100u * 61u * 2u, 0);
    ImportedSample out;
    const ImportResult r = run(buildWav(s), out);
    CHECK(r.code == ImportError::TooLong);
    CHECK(r.message.find("60 s") != std::string::npos);
    CHECK(r.message.find("61.00 s") != std::string::npos);
    CHECK(out.samples.empty());
}

TEST(length_message_never_rounds_down_to_the_limit)
{
    ImportedSample out;
    ImportResult r = run(buildWav(silence16(48000, 2880001)), out);
    CHECK(r.code == ImportError::TooLong);
    CHECK(r.message.find("60.01 s") != std::string::npos);
    CHECK(r.message.find("(60.0 s)") == std::string::npos);
    CHECK(r.message.find("(60.00 s)") == std::string::npos);

    // 44.1 kHz: one frame over is 60.0000227 s, which still shows as 60.01.
    r = run(buildWav(silence16(44100, 44100u * 60u + 1u)), out);
    CHECK(r.code == ImportError::TooLong);
    CHECK(r.message.find("60.01 s") != std::string::npos);

    // Two decimals with a leading zero in the fraction.
    r = run(buildWav(silence16(8000, 8000u * 61u + 40u)), out); // 61.005 s
    CHECK(r.code == ImportError::TooLong);
    CHECK(r.message.find("61.01 s") != std::string::npos);
    r = run(buildWav(silence16(8000, 8000u * 61u)), out); // exactly 61 s
    CHECK(r.message.find("61.00 s") != std::string::npos);
}

TEST(length_boundary_at_other_rates)
{
    // Exactly 60 s is accepted and one more frame is refused, at every kind of rate. The
    // accept case needs the resampler, so it is run once per rate with silence.
    const std::uint32_t rates[] = {8000, 22050, 96000, 192000};
    for (std::uint32_t rate : rates)
    {
        ImportedSample out;
        const ImportResult over = run(buildWav(silence16(rate, 60ull * rate + 1u)), out);
        CHECK(over.code == ImportError::TooLong);
        CHECK(out.samples.empty());

        const ImportResult ok = run(buildWav(silence16(rate, 60ull * rate)), out);
        CHECK(ok.ok());
        CHECK_EQ(out.frames(), 2880000u);
    }
}

TEST(length_exactly_60s_at_44k_accepted)
{
    WavSpec s;
    s.rate = 44100;
    s.data.assign(44100u * 60u * 2u, 0);
    ImportedSample out;
    const ImportResult r = run(buildWav(s), out);
    CHECK(r.ok());
    CHECK(out.frames() <= static_cast<std::size_t>(phylo::samples::kMaxSampleFrames));
    CHECK_EQ(out.frames(), 2880000u);
}

TEST(length_header_claims_more_than_data)
{
    // The header says 61 s of data; almost nothing is there. Refused without a crash.
    WavSpec s;
    s.data.assign(100, 0);
    s.declaredDataSize = 48000ll * 61 * 2;
    ImportedSample out;
    const ImportResult r = run(buildWav(s), out);
    CHECK(!r.ok());
    CHECK(r.code == ImportError::Damaged); // the chunk runs past the end of the file
    CHECK(out.samples.empty());

    // And a data size of nearly 4 GiB.
    s.declaredDataSize = 0xFFFFFFFFll;
    CHECK(run(buildWav(s), out).code == ImportError::Damaged);
    CHECK(out.samples.empty());

    // When the data really is there, the same header is refused for its length instead.
    WavSpec full = silence16(48000, 48000ull * 61);
    CHECK(run(buildWav(full), out).code == ImportError::TooLong);
    CHECK(out.samples.empty());
}

// ---------------------------------------------------------------------------------------
// Refusals.

TEST(refuse_not_wav)
{
    expectRefusal({}, ImportError::NotWav);
    expectRefusal({'R', 'I', 'F', 'F'}, ImportError::NotWav);
    expectRefusal({'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd', '!', '!', '!'}, ImportError::NotWav);

    std::vector<std::uint8_t> avi;
    putTag(avi, "RIFF");
    put32(avi, 4);
    putTag(avi, "AVI ");
    expectRefusal(avi, ImportError::NotWav);

    ImportedSample out;
    CHECK(importWavBytes(nullptr, 0, out).code == ImportError::NotWav);
}

TEST(refuse_damaged)
{
    const std::vector<std::uint8_t> good = buildWav(pcm16(1, 48000, {1, 2, 3, 4}));

    // Truncated in the middle of the data chunk and of the fmt chunk.
    expectRefusal(std::vector<std::uint8_t>(good.begin(), good.end() - 3), ImportError::Damaged);
    expectRefusal(std::vector<std::uint8_t>(good.begin(), good.begin() + 30), ImportError::Damaged);

    WavSpec s = pcm16(1, 48000, {1, 2, 3, 4});
    s.omitFmt = true;
    expectRefusal(buildWav(s), ImportError::Damaged);

    s = pcm16(1, 48000, {1, 2, 3, 4});
    s.omitData = true;
    expectRefusal(buildWav(s), ImportError::Damaged);

    s = pcm16(1, 48000, {1, 2, 3, 4});
    s.dataBeforeFmt = true;
    expectRefusal(buildWav(s), ImportError::Damaged);

    s = pcm16(1, 48000, {1, 2, 3, 4});
    s.data.push_back(0); // 9 bytes: not whole 16-bit frames
    expectRefusal(buildWav(s), ImportError::Damaged);

    s = pcm16(2, 48000, {1, 2, 3, 4, 5, 6});
    s.data.resize(10); // 2.5 stereo frames
    expectRefusal(buildWav(s), ImportError::Damaged);

    s = pcm16(1, 48000, {1, 2, 3, 4});
    s.blockAlign = 4;
    expectRefusal(buildWav(s), ImportError::Damaged);

    s = pcm16(2, 48000, {1, 2, 3, 4});
    s.blockAlign = 2;
    expectRefusal(buildWav(s), ImportError::Damaged);

    // fmt chunk shorter than 16 bytes.
    std::vector<std::uint8_t> shortFmt;
    putTag(shortFmt, "RIFF");
    put32(shortFmt, 4 + 8 + 8);
    putTag(shortFmt, "WAVE");
    putTag(shortFmt, "fmt ");
    put32(shortFmt, 8);
    for (int i = 0; i < 8; ++i)
        shortFmt.push_back(0);
    expectRefusal(shortFmt, ImportError::Damaged);
}

TEST(refuse_unsupported_format)
{
    WavSpec s;
    s.bits = 8;
    s.data = {1, 2, 3, 4};
    expectRefusal(buildWav(s), ImportError::UnsupportedFormat);

    s = WavSpec();
    s.bits = 32; // 32-bit integer PCM
    s.data = bytesBits(5);
    expectRefusal(buildWav(s), ImportError::UnsupportedFormat);

    s = WavSpec();
    s.tag = 3;
    s.bits = 64;
    s.data.assign(16, 0);
    expectRefusal(buildWav(s), ImportError::UnsupportedFormat);

    s = WavSpec();
    s.tag = 6; // A-law
    s.bits = 8;
    s.data = {1, 2, 3, 4};
    expectRefusal(buildWav(s), ImportError::UnsupportedFormat);

    s = WavSpec();
    s.tag = 3;
    s.bits = 24; // 24-bit float
    s.data.assign(6, 0);
    ImportedSample out;
    const ImportResult r = run(buildWav(s), out);
    CHECK(r.code == ImportError::UnsupportedFormat);
    CHECK(r.message.find("24-bit float") != std::string::npos);
    CHECK(r.message.find("16-bit") != std::string::npos);
}

TEST(refuse_unsupported_channels)
{
    for (int ch : {0, 3, 6})
    {
        WavSpec s;
        s.channels = static_cast<std::uint16_t>(ch);
        s.data.assign(24, 0);
        s.blockAlign = ch * 2;
        expectRefusal(buildWav(s), ImportError::UnsupportedChannels);
    }
}

TEST(refuse_unsupported_rate)
{
    for (std::uint32_t rate : {0u, 4000u, 7999u, 192001u, 384000u})
    {
        WavSpec s;
        s.rate = rate;
        s.data.assign(8, 0);
        expectRefusal(buildWav(s), ImportError::UnsupportedRate);
    }
    // The limits themselves are fine.
    for (std::uint32_t rate : {8000u, 192000u})
    {
        WavSpec s;
        s.rate = rate;
        s.data.assign(8, 0);
        ImportedSample out;
        CHECK(run(buildWav(s), out).ok());
    }
}

TEST(refuse_empty)
{
    WavSpec s;
    expectRefusal(buildWav(s), ImportError::Empty);
}

TEST(refuse_not_finite)
{
    WavSpec s;
    s.tag = 3;
    s.bits = 32;
    s.data = bytesFloat({0.5f, std::nanf(""), 0.25f});
    expectRefusal(buildWav(s), ImportError::NotFinite);

    s.data = bytesFloat({0.5f, std::numeric_limits<float>::infinity(), 0.25f});
    expectRefusal(buildWav(s), ImportError::NotFinite);

    s.data = bytesFloat({-std::numeric_limits<float>::infinity()});
    expectRefusal(buildWav(s), ImportError::NotFinite);

    // A NaN with a payload and the sign bit set.
    s.data = bytesBits(0xFFC01234u);
    expectRefusal(buildWav(s), ImportError::NotFinite);
}

TEST(refuse_cannot_open)
{
    ImportedSample out;
    out.samples.assign(4, 1.0f);
    const std::string path =
        (std::filesystem::temp_directory_path() / "phylo_no_such_file_7f3a9c.wav").generic_string();
    const ImportResult r = importWavFile(path, out);
    CHECK(r.code == ImportError::CannotOpen);
    CHECK(!r.message.empty());
    CHECK(out.samples.empty());
}

// ---------------------------------------------------------------------------------------
// Files on disk.

namespace
{

std::string uniqueName(const char* stem)
{
    const long long t = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::string("phylo_import_") + std::to_string(t) + "_" + stem;
}

bool writeFile(const std::filesystem::path& p, const std::vector<std::uint8_t>& b)
{
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return static_cast<bool>(f);
}

std::vector<std::uint8_t> readFile(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

void roundTrip(const std::string& utf8Name)
{
    const std::filesystem::path p = std::filesystem::temp_directory_path() / std::filesystem::u8path(utf8Name);
    const std::vector<std::uint8_t> wav = buildWav(pcm16(2, 48000, {0x4000, -0x4000, -32768, 0x2000}));
    CHECK(writeFile(p, wav));

    ImportedSample out;
    const ImportResult r = importWavFile(p.u8string(), out);
    CHECK(r.ok());
    CHECK_EQ(out.channels, 2);
    CHECK_EQ(out.frames(), 2u);
    CHECK(out.samples.size() == 4 && out.samples[0] == 0.5f && out.samples[2] == -1.0f);
    CHECK(readFile(p) == wav); // the source file is untouched

    std::error_code ec;
    std::filesystem::remove(p, ec);
}

} // namespace

TEST(file_round_trip)
{
    roundTrip(uniqueName("plain.wav"));
}

TEST(file_round_trip_non_ascii_name)
{
    // UTF-8 bytes for "sample" with an a-umlaut, a space and a snowman.
    roundTrip(uniqueName("s\xC3\xA4mple \xE2\x98\x83.wav"));
}

TEST(file_too_large_refused_before_loading)
{
    // A sparse-ish file just over the largest legal size: written in chunks, refused by size.
    const std::filesystem::path p = std::filesystem::temp_directory_path() / uniqueName("big.wav");
    {
        std::ofstream f(p, std::ios::binary);
        const std::vector<char> block(1 << 20, 0);
        for (int i = 0; i < 94; ++i) // 94 MiB > 92.16 MB + 1 MiB
            f.write(block.data(), static_cast<std::streamsize>(block.size()));
    }
    ImportedSample out;
    const ImportResult r = importWavFile(p.u8string(), out);
    CHECK(r.code == ImportError::TooLong);
    CHECK(out.samples.empty());
    std::error_code ec;
    std::filesystem::remove(p, ec);
}

// ---------------------------------------------------------------------------------------
// Hostile input.

TEST(hostile_truncation_sweep)
{
    WavSpec s = pcm16(2, 44100, {100, -100, 200, -200, 300, -300, 400, -400});
    s.listBeforeData = true;
    s.oddChunkBeforeData = true;
    const std::vector<std::uint8_t> good = buildWav(s);

    for (std::size_t len = 0; len < good.size(); ++len)
    {
        ImportedSample out;
        const ImportResult r = importWavBytes(good.data(), len, out);
        if (!r.ok())
            CHECK(out.samples.empty());
        else
            CHECK(!out.samples.empty());
    }
    ImportedSample whole;
    CHECK(importWavBytes(good.data(), good.size(), whole).ok());
}

TEST(hostile_byte_flip_sweep)
{
    WavSpec s = pcm16(2, 44100, {100, -100, 200, -200, 300, -300, 400, -400});
    s.listBeforeData = true;
    const std::vector<std::uint8_t> good = buildWav(s);

    for (std::size_t i = 0; i < 64 && i < good.size(); ++i)
    {
        for (std::uint8_t v : {std::uint8_t(0x00), std::uint8_t(0xFF)})
        {
            std::vector<std::uint8_t> bad = good;
            bad[i] = v;
            ImportedSample out;
            const ImportResult r = run(bad, out);
            if (!r.ok())
                CHECK(out.samples.empty());
            else
                CHECK(out.frames() > 0);
        }
    }
}

PHYLO_TEST_MAIN
