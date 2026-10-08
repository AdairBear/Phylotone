// Wire protocol between the app and a plugin-host process (M4). JUCE-free.
//
// Every message is a frame: a 4-byte little-endian payload length, a 1-byte type,
// then the payload. The decoder rejects frames above kMaxPayloadBytes before it
// allocates, so a damaged or hostile host cannot make the app allocate without
// limit. All integers are little-endian and all floats are IEEE 754 bit patterns.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace phylo::host
{

constexpr std::uint32_t kProtocolVersion = 1;
constexpr std::uint32_t kMaxPayloadBytes = 16u * 1024u * 1024u;

enum class MsgType : std::uint8_t
{
    Hello = 1,    // host -> app: protocol version, sample rate, max block
    Load = 2,     // app -> host: plugin path, load a plugin
    Loaded = 3,   // host -> app: plugin loaded (ok) or failed (error text)
    Process = 4,  // app -> host: MIDI and planar audio input
    Audio = 5,    // host -> app: planar audio output for one Process
    Shutdown = 6, // app -> host: exit cleanly
    Error = 7,    // host -> app: a problem the app should show
};

struct Frame
{
    MsgType type = MsgType::Error;
    std::vector<std::uint8_t> payload;
};

// Builds one complete frame (length, type, payload) ready to write.
std::vector<std::uint8_t> encodeFrame(MsgType type, const std::vector<std::uint8_t>& payload);

// Reads frames from a byte stream that can arrive in pieces of any size.
class FrameDecoder
{
public:
    void feed(const std::uint8_t* data, std::size_t size);

    // Returns true and fills `out` when a whole frame is available.
    bool next(Frame& out);

    // Once true, the stream is damaged and the decoder stops producing frames.
    bool failed() const noexcept { return failed_; }
    const std::string& error() const noexcept { return error_; }

private:
    std::vector<std::uint8_t> buffer_;
    std::size_t offset_ = 0;
    bool failed_ = false;
    std::string error_;
};

// A MIDI message to deliver at a frame offset within a Process block.
struct WireMidi
{
    std::uint32_t frame = 0;
    std::uint8_t status = 0;
    std::uint8_t data1 = 0;
    std::uint8_t data2 = 0;
};

// One block of work for the plugin. Audio is planar: channel 0's frames, then
// channel 1's, and so on. audio.size() must equal channels * frames.
struct ProcessRequest
{
    std::uint32_t frames = 0;
    std::uint32_t channels = 0;
    std::vector<WireMidi> midi;
    std::vector<float> audio;
};

std::vector<std::uint8_t> encodeProcess(const ProcessRequest& req);

// Returns false if the payload is malformed or inconsistent (bad counts, sizes that
// do not match, or more than kMaxPayloadBytes).
bool decodeProcess(const std::vector<std::uint8_t>& payload, ProcessRequest& out);

// Audio output for one block: the same planar layout as the request.
std::vector<std::uint8_t> encodeAudio(std::uint32_t frames, std::uint32_t channels, const std::vector<float>& audio);
bool decodeAudio(const std::vector<std::uint8_t>& payload, std::uint32_t& frames, std::uint32_t& channels,
                 std::vector<float>& audio);

// Hello: protocol version, sample rate, and the largest block the host will accept.
std::vector<std::uint8_t> encodeHello(std::uint32_t version, std::uint32_t sampleRate, std::uint32_t maxBlock);
bool decodeHello(const std::vector<std::uint8_t>& payload, std::uint32_t& version, std::uint32_t& sampleRate,
                 std::uint32_t& maxBlock);

// Text payloads (plugin paths, error messages). Length-prefixed UTF-8, bounded by kMaxPayloadBytes.
std::vector<std::uint8_t> encodeText(const std::string& text);
bool decodeText(const std::vector<std::uint8_t>& payload, std::string& out);

} // namespace phylo::host
