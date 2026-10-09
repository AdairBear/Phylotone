#include "phylo/Wav.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>

namespace phylo {

namespace {

void put16(std::vector<std::uint8_t>& b, std::uint16_t v)
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

std::uint32_t get32(const std::uint8_t* p)
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint16_t get16(const std::uint8_t* p)
{
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

} // namespace

bool writeWavFloat(const std::string& path, const std::vector<float>& interleaved, int channels,
                   int sampleRate)
{
    if (channels < 1 || sampleRate < 1 || interleaved.size() % static_cast<std::size_t>(channels) != 0)
        return false;

    const std::uint32_t dataBytes = static_cast<std::uint32_t>(interleaved.size() * 4);
    const std::uint16_t ch = static_cast<std::uint16_t>(channels);
    const std::uint32_t rate = static_cast<std::uint32_t>(sampleRate);

    std::vector<std::uint8_t> b;
    b.reserve(44 + dataBytes);
    putTag(b, "RIFF");
    put32(b, 36 + dataBytes);
    putTag(b, "WAVE");
    putTag(b, "fmt ");
    put32(b, 16);
    put16(b, 3);                 // IEEE float
    put16(b, ch);
    put32(b, rate);
    put32(b, rate * ch * 4);     // byte rate
    put16(b, static_cast<std::uint16_t>(ch * 4)); // block align
    put16(b, 32);                // bits per sample
    putTag(b, "data");
    put32(b, dataBytes);
    for (float f : interleaved)
    {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &f, 4);
        put32(b, bits);
    }

    std::ofstream out(path, std::ios::binary);
    if (!out)
        return false;
    out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return static_cast<bool>(out);
}

bool readWavFloat(const std::string& path, std::vector<float>& interleaved, int& channels, int& sampleRate)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    const std::vector<std::uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WAVE", 4) != 0)
        return false;

    bool haveFmt = false;
    std::size_t pos = 12;
    while (pos + 8 <= b.size())
    {
        const std::uint8_t* chunk = b.data() + pos;
        const std::size_t size = get32(chunk + 4);
        const std::size_t body = pos + 8;
        if (body + size > b.size())
            return false;

        if (std::memcmp(chunk, "fmt ", 4) == 0)
        {
            if (size < 16)
                return false;
            const std::uint8_t* f = b.data() + body;
            if (get16(f) != 3 || get16(f + 14) != 32)
                return false; // only 32-bit float
            channels = get16(f + 2);
            sampleRate = static_cast<int>(get32(f + 4));
            if (channels < 1)
                return false;
            haveFmt = true;
        }
        else if (std::memcmp(chunk, "data", 4) == 0)
        {
            if (!haveFmt || size % 4 != 0)
                return false;
            interleaved.resize(size / 4);
            for (std::size_t i = 0; i < interleaved.size(); ++i)
            {
                const std::uint32_t bits = get32(b.data() + body + 4 * i);
                std::memcpy(&interleaved[i], &bits, 4);
            }
            return true;
        }
        pos = body + size + (size & 1); // chunks are word aligned
    }
    return false;
}

void TakeRecorder::begin(int channels, int sampleRate)
{
    channels_ = channels;
    sampleRate_ = sampleRate;
    samples_.clear();
}

void TakeRecorder::append(const float* interleaved, std::size_t frames)
{
    samples_.insert(samples_.end(), interleaved, interleaved + frames * static_cast<std::size_t>(channels_));
}

bool TakeRecorder::save(const std::string& path) const
{
    return writeWavFloat(path, samples_, channels_, sampleRate_);
}

} // namespace phylo
