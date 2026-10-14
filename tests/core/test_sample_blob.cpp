// Tests for the akazi.samples.v1 state blob codec (core/include/phylo/samples/SampleBlob.h).

#include "check.h"

#include "phylo/samples/SampleBlob.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace phylo::samples;

namespace
{

using Bytes = std::vector<std::uint8_t>;

float floatFromBits(std::uint32_t bits)
{
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

bool sameBits(const std::vector<float>& a, const std::vector<float>& b)
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}

bool has(const std::string& text, const char* part)
{
    return text.find(part) != std::string::npos;
}

SampleSlot makeSlot(int slot, int channels, std::size_t frames, const std::string& name = "kick", std::uint8_t root = 60)
{
    SampleSlot s;
    s.slot = slot;
    s.name = name;
    s.rootNote = root;
    s.channels = channels;
    s.samples.resize(frames * static_cast<std::size_t>(channels));
    for (std::size_t i = 0; i < s.samples.size(); ++i)
        s.samples[i] = static_cast<float>(i % 17) * 0.0625f - 0.5f;
    return s;
}

Bytes encodeOk(const SampleSlotSet& set)
{
    Bytes out;
    const BlobResult r = encodeSampleBlob(set, out);
    CHECK(r.ok());
    return out;
}

BlobResult check(const Bytes& b)
{
    return checkSampleBlob(b.data(), b.size());
}

// Builds blobs byte by byte, so a test can forge fields the encoder would never write.
struct Forge
{
    Bytes b;

    Forge& header(const char* h = kBlobHeader)
    {
        b.insert(b.end(), h, h + 16);
        return *this;
    }
    Forge& u32(std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        return *this;
    }
    Forge& u8(std::uint8_t v)
    {
        b.push_back(v);
        return *this;
    }
    Forge& preamble(std::uint32_t rate, std::uint32_t count)
    {
        return header().u32(rate).u32(count);
    }
    Forge& slotHead(int slot, int root, int channels, int nameLen, std::uint32_t frames)
    {
        return u8(static_cast<std::uint8_t>(slot))
            .u8(static_cast<std::uint8_t>(root))
            .u8(static_cast<std::uint8_t>(channels))
            .u8(static_cast<std::uint8_t>(nameLen))
            .u32(frames);
    }
    Forge& name(const std::string& n)
    {
        b.insert(b.end(), n.begin(), n.end());
        return *this;
    }
    Forge& floats(std::size_t count, float v = 0.25f)
    {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof bits);
        for (std::size_t i = 0; i < count; ++i)
            u32(bits);
        return *this;
    }
    // A complete, valid one-frame mono slot record.
    Forge& goodSlot(int slot, const std::string& n = "x")
    {
        return slotHead(slot, 60, 1, static_cast<int>(n.size()), 1).name(n).floats(1);
    }
};

std::uint32_t u32At(const Bytes& b, std::size_t off)
{
    return static_cast<std::uint32_t>(b[off]) | (static_cast<std::uint32_t>(b[off + 1]) << 8) |
           (static_cast<std::uint32_t>(b[off + 2]) << 16) | (static_cast<std::uint32_t>(b[off + 3]) << 24);
}

} // namespace

TEST(blob_round_trip_keeps_every_field)
{
    SampleSlotSet in;
    in.push_back(makeSlot(0, 1, 100, "", 60));
    in.push_back(makeSlot(1, 2, 33, "snare", 0));
    in.push_back(makeSlot(64, 1, 7, "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x8E\xB9", 127));
    in.push_back(makeSlot(100, 2, 1, std::string(64, 'n'), 36));
    in.push_back(makeSlot(127, 1, 5, "last"));

    const Bytes blob = encodeOk(in);
    CHECK_EQ(blob.size(), sampleBlobSize(in));

    SampleSlotSet out;
    const BlobResult r = decodeSampleBlob(blob.data(), blob.size(), out);
    CHECK(r.ok());
    CHECK_EQ(out.size(), in.size());
    for (std::size_t i = 0; i < in.size() && i < out.size(); ++i)
    {
        CHECK_EQ(out[i].slot, in[i].slot);
        CHECK(out[i].name == in[i].name);
        CHECK_EQ(out[i].rootNote, in[i].rootNote);
        CHECK_EQ(out[i].channels, in[i].channels);
        CHECK_EQ(out[i].frames(), in[i].frames());
        CHECK(sameBits(out[i].samples, in[i].samples));
    }
    CHECK_EQ(out[0].rootNote, kDefaultRootNote);
    CHECK_EQ(out[3].name.size(), 64u);
}

TEST(blob_round_trip_is_bit_exact_for_awkward_floats)
{
    SampleSlot s;
    s.slot = 3;
    s.name = "edge";
    s.channels = 2;
    s.samples = {0.0f,
                 -0.0f,
                 std::numeric_limits<float>::denorm_min(),
                 -std::numeric_limits<float>::denorm_min(),
                 std::numeric_limits<float>::min(),
                 1.0f,
                 -1.0f,
                 3.5f,
                 -1000.25f,
                 std::numeric_limits<float>::max(),
                 -std::numeric_limits<float>::max(),
                 floatFromBits(0x00000001u)};
    const SampleSlotSet in{s};
    const Bytes blob = encodeOk(in);

    SampleSlotSet out;
    CHECK(decodeSampleBlob(blob.data(), blob.size(), out).ok());
    CHECK_EQ(out.size(), 1u);
    if (out.size() == 1)
    {
        CHECK(sameBits(out[0].samples, s.samples));
        // Negative zero keeps its sign bit.
        CHECK_EQ(std::memcmp(&out[0].samples[1], &s.samples[1], sizeof(float)), 0);
        CHECK(floatFromBits(0x80000000u) == 0.0f); // sanity: -0.0 == 0.0, so memcmp above matters
    }
}

TEST(blob_round_trip_accepts_the_longest_sample)
{
    SampleSlotSet in;
    in.push_back(makeSlot(9, 1, kMaxSampleFrames));
    Bytes blob;
    CHECK(encodeSampleBlob(in, blob).ok());
    SampleSlotSet out;
    CHECK(decodeSampleBlob(blob.data(), blob.size(), out).ok());
    CHECK_EQ(out.size(), 1u);
    if (out.size() == 1)
        CHECK_EQ(out[0].frames(), static_cast<std::size_t>(kMaxSampleFrames));
}

TEST(blob_byte_layout_is_as_documented)
{
    SampleSlot s;
    s.slot = 5;
    s.name = "ab";
    s.rootNote = 61;
    s.channels = 2;
    s.samples = {1.0f, -2.0f, 0.5f, 0.0f};
    const SampleSlotSet in{s};
    const Bytes b = encodeOk(in);

    CHECK_EQ(b.size(), 24u + 8u + 2u + 16u);
    CHECK_EQ(b.size(), sampleBlobSize(in));
    CHECK_EQ(std::memcmp(b.data(), "akazi.samples.v1", 16), 0);
    CHECK_EQ(b[16], 0x80);
    CHECK_EQ(b[17], 0xBB);
    CHECK_EQ(b[18], 0x00);
    CHECK_EQ(b[19], 0x00);
    CHECK_EQ(u32At(b, 16), 48000u);
    CHECK_EQ(u32At(b, 20), 1u);
    CHECK_EQ(b[24], 5);
    CHECK_EQ(b[25], 61);
    CHECK_EQ(b[26], 2);
    CHECK_EQ(b[27], 2);
    CHECK_EQ(b[28], 2); // frame count, little endian
    CHECK_EQ(b[29], 0);
    CHECK_EQ(b[30], 0);
    CHECK_EQ(b[31], 0);
    CHECK_EQ(b[32], 'a');
    CHECK_EQ(b[33], 'b');
    CHECK_EQ(u32At(b, 34), 0x3F800000u);  // 1.0f
    CHECK_EQ(b[34], 0x00);
    CHECK_EQ(b[37], 0x3F);
    CHECK_EQ(u32At(b, 38), 0xC0000000u);  // -2.0f
    CHECK_EQ(u32At(b, 42), 0x3F000000u);  // 0.5f
    CHECK_EQ(u32At(b, 46), 0x00000000u);  // 0.0f
}

TEST(encoder_sorts_slots_ascending)
{
    SampleSlotSet in;
    in.push_back(makeSlot(90, 1, 2, "c"));
    in.push_back(makeSlot(3, 1, 2, "a"));
    in.push_back(makeSlot(50, 2, 2, "b"));
    const Bytes blob = encodeOk(in);

    SampleSlotSet out;
    CHECK(decodeSampleBlob(blob.data(), blob.size(), out).ok());
    CHECK_EQ(out.size(), 3u);
    if (out.size() == 3)
    {
        CHECK_EQ(out[0].slot, 3);
        CHECK_EQ(out[1].slot, 50);
        CHECK_EQ(out[2].slot, 90);
        CHECK(out[0].name == "a");
        CHECK(out[1].name == "b");
        CHECK(out[2].name == "c");
    }
    CHECK_EQ(in[0].slot, 90); // the input is not reordered
}

TEST(zero_slot_set_is_exactly_the_preamble)
{
    const Bytes blob = encodeOk({});
    CHECK_EQ(blob.size(), 24u);
    CHECK_EQ(u32At(blob, 20), 0u);

    SampleSlotSet out;
    out.push_back(makeSlot(1, 1, 1));
    CHECK(decodeSampleBlob(blob.data(), blob.size(), out).ok());
    CHECK(out.empty());
}

TEST(unknown_header_is_refused)
{
    Forge f;
    f.preamble(48000, 0);
    f.b[15] = '2'; // akazi.samples.v2
    BlobResult r = check(f.b);
    CHECK(r.code == BlobError::UnknownHeader);
    CHECK(r.message == "unknown format header");

    const Bytes garbage(40, 0x5A);
    CHECK(check(garbage).code == BlobError::UnknownHeader);
    CHECK(check({}).code == BlobError::UnknownHeader);
    const Bytes shortHeader(kBlobHeader, kBlobHeader + 15);
    CHECK(check(shortHeader).code == BlobError::UnknownHeader);
    CHECK(checkSampleBlob(nullptr, 0).code == BlobError::UnknownHeader);
}

TEST(header_right_but_preamble_incomplete_is_truncated)
{
    for (std::size_t n = 16; n < 24; ++n)
    {
        Forge f;
        f.preamble(48000, 0);
        f.b.resize(n);
        CHECK(check(f.b).code == BlobError::Truncated);
    }
}

TEST(sample_over_the_length_limit_is_refused_by_the_encoder)
{
    SampleSlotSet in;
    in.push_back(makeSlot(4, 1, static_cast<std::size_t>(kMaxSampleFrames) + 1));
    Bytes out{1, 2, 3};
    const BlobResult r = encodeSampleBlob(in, out);
    CHECK(r.code == BlobError::SampleTooLong);
    CHECK(r.message == "slot 4 is longer than 60 s");
    CHECK(out.empty());
}

TEST(sample_over_the_length_limit_is_refused_by_the_decoder)
{
    Forge f;
    f.preamble(48000, 1).slotHead(2, 60, 1, 0, kMaxSampleFrames + 1);
    const BlobResult r = check(f.b); // no data behind it: still SampleTooLong, not Truncated
    CHECK(r.code == BlobError::SampleTooLong);
    CHECK(has(r.message, "slot 2"));

    Forge g;
    g.preamble(48000, 1).slotHead(2, 60, 1, 0, 0xFFFFFFFFu);
    CHECK(check(g.b).code == BlobError::SampleTooLong);
}

TEST(slot_number_out_of_range_is_refused)
{
    for (int slot : {128, 255})
    {
        Forge f;
        f.preamble(48000, 1).slotHead(slot, 60, 1, 0, 1).floats(1);
        const BlobResult r = check(f.b);
        CHECK(r.code == BlobError::SlotOutOfRange);
        CHECK(r.message == "slot " + std::to_string(slot) + " is out of range (0 to 127)");
    }
    for (int slot : {-1, 128, 1000})
    {
        SampleSlotSet in{makeSlot(slot, 1, 4)};
        Bytes out;
        const BlobResult r = encodeSampleBlob(in, out);
        CHECK(r.code == BlobError::SlotOutOfRange);
        CHECK(has(r.message, "out of range (0 to 127)"));
        CHECK(out.empty());
    }
}

TEST(sample_rate_other_than_48k_is_refused)
{
    for (std::uint32_t rate : {44100u, 0u, 96000u, 0xFFFFFFFFu})
    {
        Forge f;
        f.preamble(rate, 0);
        const BlobResult r = check(f.b);
        CHECK(r.code == BlobError::WrongSampleRate);
        CHECK(r.message == "sample rate " + std::to_string(rate) + " Hz is not 48000 Hz");
    }
}

TEST(truncating_a_valid_blob_anywhere_is_refused)
{
    SampleSlotSet in;
    in.push_back(makeSlot(2, 2, 5, "snare"));
    in.push_back(makeSlot(77, 1, 3, "hat"));
    const Bytes blob = encodeOk(in);

    for (std::size_t len = 0; len < blob.size(); ++len)
    {
        const BlobResult r = checkSampleBlob(blob.data(), len);
        CHECK(!r.ok());
        if (len < 16)
            CHECK(r.code == BlobError::UnknownHeader);
        else
            CHECK(r.code == BlobError::Truncated);
        CHECK(!r.message.empty());

        SampleSlotSet out;
        out.push_back(makeSlot(1, 1, 1));
        const BlobResult d = decodeSampleBlob(blob.data(), len, out);
        CHECK(d.code == r.code);
        CHECK(out.empty());
    }
    CHECK(check(blob).ok());
}

TEST(too_many_slots_is_refused)
{
    Forge f;
    f.preamble(48000, 129);
    const BlobResult r = check(f.b);
    CHECK(r.code == BlobError::TooManySlots);
    CHECK(r.message == "too many slots (129)");

    Forge g;
    g.preamble(48000, 0xFFFFFFFFu);
    CHECK(check(g.b).code == BlobError::TooManySlots);

    SampleSlotSet in;
    for (int i = 0; i < 129; ++i)
        in.push_back(makeSlot(i % 128, 1, 1));
    Bytes out;
    CHECK(encodeSampleBlob(in, out).code == BlobError::TooManySlots);
}

TEST(a_full_set_of_128_slots_is_accepted)
{
    SampleSlotSet in;
    for (int i = 127; i >= 0; --i)
        in.push_back(makeSlot(i, 1, 3));
    const Bytes blob = encodeOk(in);
    SampleSlotSet out;
    CHECK(decodeSampleBlob(blob.data(), blob.size(), out).ok());
    CHECK_EQ(out.size(), 128u);
}

TEST(slot_order_is_enforced)
{
    Forge dup;
    dup.preamble(48000, 2).goodSlot(5).goodSlot(5);
    BlobResult r = check(dup.b);
    CHECK(r.code == BlobError::SlotOrder);
    CHECK(r.message == "slot 5 appears twice");

    Forge down;
    down.preamble(48000, 2).goodSlot(9).goodSlot(4);
    r = check(down.b);
    CHECK(r.code == BlobError::SlotOrder);
    CHECK(r.message == "slot 4 is out of order");

    SampleSlotSet in;
    in.push_back(makeSlot(8, 1, 2));
    in.push_back(makeSlot(1, 1, 2));
    in.push_back(makeSlot(8, 1, 2));
    Bytes out;
    r = encodeSampleBlob(in, out);
    CHECK(r.code == BlobError::SlotOrder);
    CHECK(r.message == "slot 8 appears twice");
    CHECK(out.empty());
}

TEST(root_note_above_127_is_refused)
{
    Forge f;
    f.preamble(48000, 1).slotHead(3, 128, 1, 0, 1).floats(1);
    const BlobResult r = check(f.b);
    CHECK(r.code == BlobError::BadRootNote);
    CHECK(r.message == "slot 3 root note 128 is out of range (0 to 127)");

    SampleSlotSet in{makeSlot(3, 1, 2, "x", 200)};
    Bytes out;
    CHECK(encodeSampleBlob(in, out).code == BlobError::BadRootNote);
}

TEST(channel_count_must_be_one_or_two)
{
    for (int ch : {0, 3, 255})
    {
        Forge f;
        f.preamble(48000, 1).slotHead(6, 60, ch, 0, 1).floats(4);
        const BlobResult r = check(f.b);
        CHECK(r.code == BlobError::BadChannelCount);
        CHECK(r.message == "slot 6 has " + std::to_string(ch) + " channels (1 or 2 allowed)");
    }
    for (int ch : {0, 3, -1})
    {
        SampleSlotSet in{makeSlot(6, 1, 6)};
        in[0].channels = ch;
        Bytes out;
        CHECK(encodeSampleBlob(in, out).code == BlobError::BadChannelCount);
        CHECK(out.empty());
    }
}

TEST(name_longer_than_64_bytes_is_refused)
{
    Forge f;
    f.preamble(48000, 1).slotHead(1, 60, 1, 65, 1).name(std::string(65, 'a')).floats(1);
    const BlobResult r = check(f.b);
    CHECK(r.code == BlobError::NameTooLong);
    CHECK(r.message == "slot 1 name is longer than 64 bytes");

    SampleSlotSet in{makeSlot(1, 1, 2, std::string(65, 'a'))};
    Bytes out;
    CHECK(encodeSampleBlob(in, out).code == BlobError::NameTooLong);

    SampleSlotSet ok{makeSlot(1, 1, 2, std::string(64, 'a'))};
    CHECK(encodeSampleBlob(ok, out).ok());
}

TEST(sample_with_no_frames_is_refused)
{
    Forge f;
    f.preamble(48000, 1).slotHead(12, 60, 2, 0, 0);
    const BlobResult r = check(f.b);
    CHECK(r.code == BlobError::EmptySample);
    CHECK(r.message == "slot 12 has no frames");

    SampleSlotSet in{makeSlot(12, 1, 0)};
    Bytes out;
    CHECK(encodeSampleBlob(in, out).code == BlobError::EmptySample);
}

TEST(name_must_be_valid_utf8)
{
    const char* bad[] = {"\xFF", "\xC0\xAF", "\xC1\xBF", "\xE0\x80\x80", "\xED\xA0\x80", "\xF4\x90\x80\x80",
                         "\xF5\x80\x80\x80", "\xC3", "\xE2\x82", "ok\x80", "\x80"};
    for (const char* n : bad)
    {
        const std::string name(n);
        Forge f;
        f.preamble(48000, 1).slotHead(7, 60, 1, static_cast<int>(name.size()), 1).name(name).floats(1);
        const BlobResult r = check(f.b);
        CHECK(r.code == BlobError::BadName);
        CHECK(r.message == "slot 7 name is not valid UTF-8");

        SampleSlotSet in{makeSlot(7, 1, 2, name)};
        Bytes out;
        CHECK(encodeSampleBlob(in, out).code == BlobError::BadName);
    }
    // Valid multi-byte sequences at their limits are accepted.
    const char* good[] = {"\xC2\x80", "\xDF\xBF", "\xE0\xA0\x80", "\xED\x9F\xBF", "\xEE\x80\x80", "\xEF\xBF\xBF",
                          "\xF0\x90\x80\x80", "\xF4\x8F\xBF\xBF"};
    for (const char* n : good)
    {
        SampleSlotSet in{makeSlot(7, 1, 2, n)};
        Bytes out;
        CHECK(encodeSampleBlob(in, out).ok());
    }
}

TEST(name_utf8_overlongs_surrogates_and_strays_are_refused)
{
    const char* bad[] = {"\xF0\x80\x80\x80",         // 4-byte overlong NUL
                         "\xF0\x8F\xBF\xBF",         // 4-byte overlong, the largest one
                         "\xED\xA0\x80",             // UTF-16 surrogate U+D800
                         "\xED\xBF\xBF",             // U+DFFF
                         "\xF4\x90\x80\x80",         // above U+10FFFF
                         "\x80",                     // lone continuation byte
                         "a\xBF",                    // continuation after ASCII
                         "\xC3\xA9\x80"};            // continuation after a full character
    for (const char* n : bad)
    {
        const std::string name(n);
        Forge f;
        f.preamble(48000, 1).slotHead(7, 60, 1, static_cast<int>(name.size()), 1).name(name).floats(1);
        const BlobResult r = check(f.b);
        CHECK(r.code == BlobError::BadName);
        SampleSlotSet out;
        CHECK(decodeSampleBlob(f.b.data(), f.b.size(), out).code == BlobError::BadName);
        CHECK(out.empty());

        SampleSlotSet in{makeSlot(7, 1, 2, name)};
        Bytes enc;
        CHECK(encodeSampleBlob(in, enc).code == BlobError::BadName);
    }
}

TEST(name_with_a_four_byte_character_round_trips)
{
    const std::string name = "grin \xF0\x9F\x98\x80"; // U+1F600
    SampleSlotSet in{makeSlot(7, 1, 2, name)};
    const Bytes blob = encodeOk(in);
    SampleSlotSet out;
    CHECK(decodeSampleBlob(blob.data(), blob.size(), out).ok());
    CHECK_EQ(out.size(), 1u);
    if (out.size() == 1)
        CHECK(out[0].name == name);
    // The same name as the whole 4-byte character alone.
    SampleSlotSet lone{makeSlot(8, 1, 2, "\xF0\x9F\x98\x80")};
    const Bytes blob2 = encodeOk(lone);
    SampleSlotSet out2;
    CHECK(decodeSampleBlob(blob2.data(), blob2.size(), out2).ok());
    CHECK_EQ(out2.size(), 1u);
    if (out2.size() == 1)
        CHECK_EQ(out2[0].name.size(), 4u);
}

TEST(total_size_over_the_budget_is_refused_before_anything_else_after_the_header)
{
    // One ~64 MiB zero-filled buffer, reused: header and preamble written at the front.
    Bytes big(kMaxBlobBytes + 1, 0);
    auto setPreamble = [&](std::uint32_t rate, std::uint32_t count)
    {
        std::memcpy(big.data(), kBlobHeader, kBlobHeaderBytes);
        for (int i = 0; i < 4; ++i)
        {
            big[16 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(rate >> (8 * i));
            big[20 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(count >> (8 * i));
        }
    };

    // One byte over, valid header and preamble.
    setPreamble(48000, 0);
    BlobResult r = checkSampleBlob(big.data(), big.size());
    CHECK(r.code == BlobError::SetTooLarge);
    SampleSlotSet out;
    out.push_back(makeSlot(0, 1, 1));
    CHECK(decodeSampleBlob(big.data(), big.size(), out).code == BlobError::SetTooLarge);
    CHECK(out.empty());

    // One byte over and a wrong sample rate: the size is reported first.
    setPreamble(44100, 0);
    CHECK(checkSampleBlob(big.data(), big.size()).code == BlobError::SetTooLarge);

    // A wrong header still comes before the size.
    setPreamble(48000, 0);
    big[15] = '9';
    CHECK(checkSampleBlob(big.data(), big.size()).code == BlobError::UnknownHeader);
    big[15] = '1';

    // Exactly the limit is not too large; it fails on what follows the preamble instead.
    r = checkSampleBlob(big.data(), kMaxBlobBytes);
    CHECK(r.code != BlobError::SetTooLarge);
    CHECK(r.code == BlobError::TrailingBytes);
    setPreamble(44100, 0);
    r = checkSampleBlob(big.data(), kMaxBlobBytes);
    CHECK(r.code == BlobError::WrongSampleRate);
}

TEST(samples_must_be_finite)
{
    const float values[] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                            -std::numeric_limits<float>::infinity(), floatFromBits(0xFFC00000u),
                            floatFromBits(0x7F800001u)};
    for (float v : values)
    {
        SampleSlotSet in{makeSlot(10, 2, 4)};
        in[0].samples[5] = v;
        Bytes out;
        const BlobResult r = encodeSampleBlob(in, out);
        CHECK(r.code == BlobError::NotFinite);
        CHECK(r.message == "slot 10 has a sample that is not a finite number");
        CHECK(out.empty());

        Forge f;
        f.preamble(48000, 1).slotHead(10, 60, 1, 0, 3).floats(2).floats(1, v);
        CHECK(check(f.b).code == BlobError::NotFinite);
    }
}

TEST(bytes_after_the_last_slot_are_refused)
{
    Bytes blob = encodeOk({makeSlot(1, 1, 3)});
    blob.push_back(0);
    const BlobResult r = check(blob);
    CHECK(r.code == BlobError::TrailingBytes);
    CHECK(r.message == "unexpected bytes after the last slot");

    Bytes empty = encodeOk({});
    empty.push_back(0);
    CHECK(check(empty).code == BlobError::TrailingBytes);
}

TEST(set_over_the_budget_is_refused_without_encoding)
{
    // Three stereo 60 s samples are about 69 MB, over the 64 MiB limit.
    SampleSlotSet in;
    in.push_back(makeSlot(0, 2, kMaxSampleFrames));
    in.push_back(in[0]);
    in.push_back(in[0]);
    in[1].slot = 1;
    in[2].slot = 2;
    CHECK(sampleBlobSize(in) > kMaxBlobBytes);

    Bytes out{9};
    const BlobResult r = encodeSampleBlob(in, out);
    CHECK(r.code == BlobError::SetTooLarge);
    CHECK(has(r.message, "sample set is 6"));
    CHECK(has(r.message, "MB; the limit is 64 MB"));
    CHECK(out.empty());
    CHECK(out.capacity() <= 1);

    in.pop_back();
    CHECK(sampleBlobSize(in) <= kMaxBlobBytes);
    CHECK(encodeSampleBlob(in, out).ok());
}

TEST(sample_count_must_match_channel_count)
{
    SampleSlotSet in{makeSlot(14, 2, 4)};
    in[0].samples.pop_back();
    Bytes out;
    const BlobResult r = encodeSampleBlob(in, out);
    CHECK(r.code == BlobError::BadSampleCount);
    CHECK(r.message == "slot 14 sample count is not a multiple of its channel count");
    CHECK(out.empty());

    in[0].samples.resize(1); // one sample, two channels: zero whole frames
    CHECK(encodeSampleBlob(in, out).code == BlobError::BadSampleCount);
}

TEST(check_order_reports_the_earlier_problem)
{
    // Wrong header and wrong rate: header wins.
    Forge a;
    a.header("akazi.samples.v2").u32(44100).u32(0);
    CHECK(check(a.b).code == BlobError::UnknownHeader);

    // Wrong rate and too many slots: rate wins.
    Forge b;
    b.preamble(44100, 500);
    CHECK(check(b.b).code == BlobError::WrongSampleRate);

    // Too many slots and truncated: slot count wins.
    Forge c;
    c.preamble(48000, 200);
    CHECK(check(c.b).code == BlobError::TooManySlots);

    // Slot out of range and bad channels: slot range wins.
    Forge d;
    d.preamble(48000, 1).slotHead(200, 60, 3, 0, 1);
    CHECK(check(d.b).code == BlobError::SlotOutOfRange);

    // Bad root note and bad channels: root note wins.
    Forge e;
    e.preamble(48000, 1).slotHead(1, 200, 3, 0, 1);
    CHECK(check(e.b).code == BlobError::BadRootNote);

    // Bad channels and long name: channels win.
    Forge f;
    f.preamble(48000, 1).slotHead(1, 60, 0, 90, 1);
    CHECK(check(f.b).code == BlobError::BadChannelCount);

    // Long name and zero frames: name length wins.
    Forge g;
    g.preamble(48000, 1).slotHead(1, 60, 1, 70, 0);
    CHECK(check(g.b).code == BlobError::NameTooLong);

    // Over-long and truncated: over-long wins.
    Forge h;
    h.preamble(48000, 1).slotHead(1, 60, 1, 5, kMaxSampleFrames + 1).name("ab");
    CHECK(check(h.b).code == BlobError::SampleTooLong);

    // Truncated and bad name: truncated wins (the name bytes are not all there).
    Forge i;
    i.preamble(48000, 1).slotHead(1, 60, 1, 2, 4).name("\xFF\xFF").floats(2);
    CHECK(check(i.b).code == BlobError::Truncated);

    // Bad name and NaN: name wins.
    Forge j;
    j.preamble(48000, 1).slotHead(1, 60, 1, 1, 1).name("\xFF").floats(1, std::numeric_limits<float>::quiet_NaN());
    CHECK(check(j.b).code == BlobError::BadName);

    // NaN in the first slot and trailing bytes: NaN wins.
    Forge k;
    k.preamble(48000, 1).slotHead(1, 60, 1, 0, 1).floats(1, std::numeric_limits<float>::infinity()).u8(0);
    CHECK(check(k.b).code == BlobError::NotFinite);

    // A problem in slot 1 beats one in slot 2.
    Forge l;
    l.preamble(48000, 2).slotHead(1, 60, 1, 0, 1).floats(1, std::numeric_limits<float>::infinity()).slotHead(1, 60, 1, 0, 1);
    CHECK(check(l.b).code == BlobError::NotFinite);
}

TEST(blob_faults_produce_their_matching_error)
{
    const Bytes good = encodeOk({makeSlot(4, 2, 6, "pad"), makeSlot(9, 1, 3, "bass")});
    CHECK(check(good).ok());

    struct Case
    {
        BlobFault fault;
        BlobError error;
    };
    const Case cases[] = {{BlobFault::WrongHeader, BlobError::UnknownHeader},
                          {BlobFault::WrongSampleRate, BlobError::WrongSampleRate},
                          {BlobFault::SlotOutOfRange, BlobError::SlotOutOfRange},
                          {BlobFault::SampleTooLong, BlobError::SampleTooLong},
                          {BlobFault::Truncated, BlobError::Truncated}};
    for (const Case& c : cases)
    {
        Bytes blob = good;
        CHECK(applyBlobFaultForTesting(blob, c.fault));
        CHECK(check(blob).code == c.error);
    }

    Bytes blob = good;
    CHECK(applyBlobFaultForTesting(blob, BlobFault::Truncated));
    CHECK_EQ(blob.size(), good.size() - 4);
    blob = good;
    CHECK(applyBlobFaultForTesting(blob, BlobFault::WrongHeader));
    CHECK_EQ(blob[15], '9');
    blob = good;
    CHECK(applyBlobFaultForTesting(blob, BlobFault::SampleTooLong));
    CHECK_EQ(u32At(blob, 28), kMaxSampleFrames + 1);
}

TEST(blob_faults_on_an_empty_set)
{
    const Bytes empty = encodeOk({});
    Bytes blob = empty;
    CHECK(!applyBlobFaultForTesting(blob, BlobFault::SlotOutOfRange));
    CHECK(blob == empty);
    CHECK(!applyBlobFaultForTesting(blob, BlobFault::SampleTooLong));
    CHECK(blob == empty);

    CHECK(applyBlobFaultForTesting(blob, BlobFault::WrongHeader));
    CHECK(check(blob).code == BlobError::UnknownHeader);

    blob = empty;
    CHECK(applyBlobFaultForTesting(blob, BlobFault::WrongSampleRate));
    CHECK(check(blob).code == BlobError::WrongSampleRate);

    blob = empty;
    CHECK(applyBlobFaultForTesting(blob, BlobFault::Truncated));
    CHECK_EQ(blob.size(), 20u);
    CHECK(check(blob).code == BlobError::Truncated);

    Bytes tiny{1, 2};
    CHECK(!applyBlobFaultForTesting(tiny, BlobFault::WrongHeader));
    CHECK(!applyBlobFaultForTesting(tiny, BlobFault::Truncated));
}

TEST(looks_like_sample_blob_matches_the_family_prefix)
{
    const Bytes good = encodeOk({makeSlot(1, 1, 2)});
    CHECK(looksLikeSampleBlob(good.data(), good.size()));

    Bytes v9 = good;
    v9[15] = '9';
    CHECK(looksLikeSampleBlob(v9.data(), v9.size()));

    const char* xml = "<?xml version=\"1.0\"?><state/>";
    CHECK(!looksLikeSampleBlob(reinterpret_cast<const std::uint8_t*>(xml), std::strlen(xml)));
    CHECK(!looksLikeSampleBlob(nullptr, 0));
    CHECK(!looksLikeSampleBlob(good.data(), 13));
    CHECK(looksLikeSampleBlob(good.data(), 14));
}

TEST(sample_blob_refusal_gives_a_reason_only_for_bad_sample_blobs)
{
    const Bytes good = encodeOk({makeSlot(1, 1, 2)});
    CHECK(sampleBlobRefusal(good.data(), good.size()).empty());

    Bytes v9 = good;
    v9[15] = '9';
    CHECK(sampleBlobRefusal(v9.data(), v9.size()) == "unknown format header");

    Bytes cut = good;
    cut.pop_back();
    CHECK(sampleBlobRefusal(cut.data(), cut.size()) == "truncated blob");

    Bytes bad = good;
    CHECK(applyBlobFaultForTesting(bad, BlobFault::SlotOutOfRange));
    CHECK(sampleBlobRefusal(bad.data(), bad.size()) == "slot 128 is out of range (0 to 127)");

    const char* xml = "<?xml version=\"1.0\"?>";
    CHECK(sampleBlobRefusal(reinterpret_cast<const std::uint8_t*>(xml), std::strlen(xml)).empty());
    CHECK(sampleBlobRefusal(nullptr, 0).empty());
    const Bytes none;
    CHECK(sampleBlobRefusal(none.data(), none.size()).empty());
}

TEST(sanitise_sample_name_makes_a_safe_name)
{
    CHECK(sanitiseSampleName("kick") == "kick");
    CHECK(sanitiseSampleName("  kick drum  ") == "kick drum");
    CHECK(sanitiseSampleName("") == "sample");
    CHECK(sanitiseSampleName("   ") == "sample");
    CHECK(sanitiseSampleName(std::string("a\0b", 3)) == "ab");
    CHECK(sanitiseSampleName("a\tb\nc\x1F" "d\x7F" "e") == "abcde");
    CHECK(sanitiseSampleName("\n\t") == "sample");
    CHECK(sanitiseSampleName("a\xFF" "b") == "ab");
    CHECK(sanitiseSampleName("\xC0\xAF") == "sample");
    CHECK(sanitiseSampleName("\xED\xA0\x80x") == "x");
    CHECK(sanitiseSampleName("caf\xC3\xA9") == "caf\xC3\xA9");
    CHECK(sanitiseSampleName("caf\xC3") == "caf"); // cut-short sequence is dropped

    // 64 bytes of ASCII stay; 65 are cut to 64.
    CHECK(sanitiseSampleName(std::string(64, 'a')) == std::string(64, 'a'));
    CHECK(sanitiseSampleName(std::string(100, 'a')) == std::string(64, 'a'));

    // A two-byte character straddling the limit is dropped whole.
    std::string straddle(63, 'a');
    straddle += "\xC3\xA9";
    straddle += "tail";
    CHECK(sanitiseSampleName(straddle) == std::string(63, 'a'));

    // A four-byte character straddling the limit.
    std::string four(62, 'a');
    four += "\xF0\x9F\x8E\xB9";
    CHECK(sanitiseSampleName(four) == std::string(62, 'a'));

    // The cut can leave a trailing space, which is trimmed too.
    CHECK(sanitiseSampleName(std::string(63, 'a') + " " + "bbbb") == std::string(63, 'a'));

    // Whatever comes out is accepted by the blob.
    const std::string messy = std::string("\xFF\x01  ") + std::string(70, '\xC3') + "\xA9" + std::string(30, 'z');
    SampleSlotSet in{makeSlot(0, 1, 2, sanitiseSampleName(messy))};
    Bytes out;
    CHECK(encodeSampleBlob(in, out).ok());
    CHECK(in[0].name.size() <= kMaxNameBytes);
}

TEST(a_refused_decode_leaves_the_output_empty)
{
    Bytes blob = encodeOk({makeSlot(1, 1, 3), makeSlot(2, 1, 3)});
    blob.push_back(7);
    SampleSlotSet out;
    out.push_back(makeSlot(5, 1, 1));
    const BlobResult r = decodeSampleBlob(blob.data(), blob.size(), out);
    CHECK(r.code == BlobError::TrailingBytes);
    CHECK(out.empty());
}

TEST(hostile_single_byte_changes_never_crash_and_check_agrees_with_decode)
{
    const Bytes base = encodeOk({makeSlot(3, 2, 4, "pad"), makeSlot(20, 1, 3, "caf\xC3\xA9")});
    const std::uint8_t values[] = {0x00, 0xFF, 0x80, 0x7F};
    int accepted = 0;
    int refused = 0;
    for (std::size_t i = 0; i < base.size(); ++i)
    {
        for (std::uint8_t v : values)
        {
            Bytes blob = base;
            blob[i] = v;
            const BlobResult c = checkSampleBlob(blob.data(), blob.size());
            SampleSlotSet out;
            const BlobResult d = decodeSampleBlob(blob.data(), blob.size(), out);
            CHECK(c.code == d.code);
            CHECK(c.message == d.message);
            if (c.ok())
            {
                ++accepted;
                CHECK(!out.empty());
                // A decoded set re-encodes to the same bytes.
                Bytes again;
                CHECK(encodeSampleBlob(out, again).ok());
                CHECK(again == blob);
            }
            else
            {
                ++refused;
                CHECK(out.empty());
                CHECK(!c.message.empty());
            }
        }
    }
    CHECK(refused > 0);
    CHECK(accepted > 0); // sample bytes and some name bytes can change freely
}

PHYLO_TEST_MAIN
