#include "phylo/host/Wire.h"

#include <cstring>

namespace phylo::host
{

namespace
{

void putU32(std::vector<std::uint8_t>& out, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

std::uint32_t getU32(const std::uint8_t* p)
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

void putFloat(std::vector<std::uint8_t>& out, float f)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof bits);
    putU32(out, bits);
}

float getFloat(const std::uint8_t* p)
{
    const std::uint32_t bits = getU32(p);
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

// Reads a u32 at `pos` if there are four bytes left.
bool readU32(const std::vector<std::uint8_t>& in, std::size_t& pos, std::uint32_t& v)
{
    if (pos > in.size() || in.size() - pos < 4)
        return false;
    v = getU32(in.data() + pos);
    pos += 4;
    return true;
}

} // namespace

std::vector<std::uint8_t> encodeFrame(MsgType type, const std::vector<std::uint8_t>& payload)
{
    std::vector<std::uint8_t> out;
    out.reserve(5 + payload.size());
    putU32(out, static_cast<std::uint32_t>(payload.size()));
    out.push_back(static_cast<std::uint8_t>(type));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

void FrameDecoder::feed(const std::uint8_t* data, std::size_t size)
{
    if (failed_ || size == 0)
        return;
    buffer_.insert(buffer_.end(), data, data + size);
}

bool FrameDecoder::next(Frame& out)
{
    if (failed_)
        return false;

    // Drop consumed bytes now and then so the buffer does not grow without limit.
    if (offset_ > 0 && offset_ * 2 >= buffer_.size())
    {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(offset_));
        offset_ = 0;
    }

    const std::size_t available = buffer_.size() - offset_;
    if (available < 5)
        return false;

    const std::uint32_t length = getU32(buffer_.data() + offset_);
    if (length > kMaxPayloadBytes)
    {
        failed_ = true;
        error_ = "frame too large: " + std::to_string(length) + " bytes";
        return false;
    }
    if (available < 5 + static_cast<std::size_t>(length))
        return false;

    const std::uint8_t type = buffer_[offset_ + 4];
    if (type < static_cast<std::uint8_t>(MsgType::Hello) || type > static_cast<std::uint8_t>(MsgType::StateResult))
    {
        failed_ = true;
        error_ = "unknown message type: " + std::to_string(type);
        return false;
    }

    out.type = static_cast<MsgType>(type);
    const auto begin = buffer_.begin() + static_cast<std::ptrdiff_t>(offset_ + 5);
    out.payload.assign(begin, begin + static_cast<std::ptrdiff_t>(length));
    offset_ += 5 + length;
    return true;
}

std::vector<std::uint8_t> encodeProcess(const ProcessRequest& req)
{
    std::vector<std::uint8_t> out;
    putU32(out, req.frames);
    putU32(out, req.channels);
    putU32(out, static_cast<std::uint32_t>(req.midi.size()));
    for (const auto& m : req.midi)
    {
        putU32(out, m.frame);
        out.push_back(m.status);
        out.push_back(m.data1);
        out.push_back(m.data2);
    }
    for (float f : req.audio)
        putFloat(out, f);
    return out;
}

bool decodeProcess(const std::vector<std::uint8_t>& payload, ProcessRequest& out)
{
    std::size_t pos = 0;
    std::uint32_t frames = 0, channels = 0, midiCount = 0;
    if (!readU32(payload, pos, frames) || !readU32(payload, pos, channels) || !readU32(payload, pos, midiCount))
        return false;

    // Each MIDI event takes 7 bytes, so the count must fit in what is left.
    if (midiCount > (payload.size() - pos) / 7)
        return false;

    ProcessRequest r;
    r.frames = frames;
    r.channels = channels;
    r.midi.reserve(midiCount);
    for (std::uint32_t i = 0; i < midiCount; ++i)
    {
        WireMidi m;
        if (!readU32(payload, pos, m.frame))
            return false;
        m.status = payload[pos++];
        m.data1 = payload[pos++];
        m.data2 = payload[pos++];
        r.midi.push_back(m);
    }

    // Audio must be exactly channels * frames floats, with no bytes left over.
    const std::size_t remaining = payload.size() - pos;
    if (remaining % 4 != 0)
        return false;
    const std::uint64_t samples = static_cast<std::uint64_t>(frames) * channels;
    if (samples * 4 != remaining)
        return false;

    r.audio.resize(static_cast<std::size_t>(samples));
    for (std::size_t i = 0; i < r.audio.size(); ++i)
        r.audio[i] = getFloat(payload.data() + pos + 4 * i);

    out = std::move(r);
    return true;
}

std::vector<std::uint8_t> encodeAudio(std::uint32_t frames, std::uint32_t channels, const std::vector<float>& audio)
{
    std::vector<std::uint8_t> out;
    putU32(out, frames);
    putU32(out, channels);
    for (float f : audio)
        putFloat(out, f);
    return out;
}

bool decodeAudio(const std::vector<std::uint8_t>& payload, std::uint32_t& frames, std::uint32_t& channels,
                 std::vector<float>& audio)
{
    std::size_t pos = 0;
    std::uint32_t f = 0, c = 0;
    if (!readU32(payload, pos, f) || !readU32(payload, pos, c))
        return false;
    const std::size_t remaining = payload.size() - pos;
    const std::uint64_t samples = static_cast<std::uint64_t>(f) * c;
    if (remaining % 4 != 0 || samples * 4 != remaining)
        return false;
    audio.resize(static_cast<std::size_t>(samples));
    for (std::size_t i = 0; i < audio.size(); ++i)
        audio[i] = getFloat(payload.data() + pos + 4 * i);
    frames = f;
    channels = c;
    return true;
}

std::vector<std::uint8_t> encodeHello(std::uint32_t version, std::uint32_t sampleRate, std::uint32_t maxBlock)
{
    std::vector<std::uint8_t> out;
    putU32(out, version);
    putU32(out, sampleRate);
    putU32(out, maxBlock);
    return out;
}

bool decodeHello(const std::vector<std::uint8_t>& payload, std::uint32_t& version, std::uint32_t& sampleRate,
                 std::uint32_t& maxBlock)
{
    std::size_t pos = 0;
    return readU32(payload, pos, version) && readU32(payload, pos, sampleRate) &&
           readU32(payload, pos, maxBlock) && pos == payload.size();
}

std::vector<std::uint8_t> encodeText(const std::string& text)
{
    std::vector<std::uint8_t> out(text.begin(), text.end());
    return out;
}

bool decodeText(const std::vector<std::uint8_t>& payload, std::string& out)
{
    if (payload.size() > kMaxPayloadBytes)
        return false;
    out.assign(payload.begin(), payload.end());
    return true;
}

} // namespace phylo::host
