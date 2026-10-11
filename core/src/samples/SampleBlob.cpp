#include "phylo/samples/SampleBlob.h"

#include <algorithm>
#include <cstring>
#include <numeric>

namespace phylo::samples
{

namespace
{

BlobResult refuse(BlobError code, std::string message)
{
    BlobResult r;
    r.code = code;
    r.message = std::move(message);
    return r;
}

std::string slotText(int slot)
{
    return "slot " + std::to_string(slot);
}

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

// Finite means the exponent field is not all ones. This tests the bit pattern on purpose:
// std::isfinite may be folded away under -ffast-math.
bool bitsAreFinite(std::uint32_t bits)
{
    return (bits & 0x7F800000u) != 0x7F800000u;
}

bool floatIsFinite(float f)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof bits);
    return bitsAreFinite(bits);
}

// Length of the valid, strict UTF-8 sequence starting at p[0], or 0 when it is invalid.
// `n` is the number of bytes available from p. Rejects overlongs, surrogates, anything
// above U+10FFFF and sequences cut short.
std::size_t utf8SequenceLength(const std::uint8_t* p, std::size_t n)
{
    if (n == 0)
        return 0;
    const unsigned b0 = p[0];
    if (b0 < 0x80)
        return 1;

    std::size_t len = 0;
    unsigned lo = 0x80;
    unsigned hi = 0xBF;
    if (b0 >= 0xC2 && b0 <= 0xDF)
        len = 2;
    else if (b0 >= 0xE0 && b0 <= 0xEF)
    {
        len = 3;
        if (b0 == 0xE0)
            lo = 0xA0;
        else if (b0 == 0xED)
            hi = 0x9F;
    }
    else if (b0 >= 0xF0 && b0 <= 0xF4)
    {
        len = 4;
        if (b0 == 0xF0)
            lo = 0x90;
        else if (b0 == 0xF4)
            hi = 0x8F;
    }
    else
        return 0;

    if (n < len)
        return 0;
    if (p[1] < lo || p[1] > hi)
        return 0;
    for (std::size_t i = 2; i < len; ++i)
        if (p[i] < 0x80 || p[i] > 0xBF)
            return 0;
    return len;
}

bool isValidUtf8(const std::uint8_t* p, std::size_t n)
{
    std::size_t i = 0;
    while (i < n)
    {
        const std::size_t len = utf8SequenceLength(p + i, n - i);
        if (len == 0)
            return false;
        i += len;
    }
    return true;
}

// Formats a byte count as megabytes (1 MB = 1024 * 1024 bytes). A whole number prints
// without a decimal; otherwise one decimal, rounded up so a set just over the limit never
// reads as equal to it.
std::string megabytes(std::size_t bytes)
{
    const std::size_t mb = std::size_t{1} << 20;
    if (bytes % mb == 0)
        return std::to_string(bytes / mb);
    const std::size_t tenths = bytes / mb * 10 + ((bytes % mb) * 10 + mb - 1) / mb;
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10);
}

BlobResult setTooLarge(std::size_t size)
{
    return refuse(BlobError::SetTooLarge, "sample set is " + megabytes(size) + " MB; the limit is " +
                                              megabytes(kMaxBlobBytes) + " MB");
}

} // namespace

std::size_t sampleBlobSize(const SampleSlotSet& slots)
{
    std::size_t total = kBlobPreambleBytes;
    for (const auto& s : slots)
        total += kSlotRecordHeadBytes + s.name.size() + 4 * s.samples.size();
    return total;
}

BlobResult checkSampleSlots(const SampleSlotSet& slots)
{
    if (slots.size() > static_cast<std::size_t>(kSlotCount))
        return refuse(BlobError::TooManySlots, "too many slots (" + std::to_string(slots.size()) + ")");

    bool seen[kSlotCount] = {};
    for (const auto& s : slots)
    {
        const std::string who = slotText(s.slot);
        if (s.slot < 0 || s.slot > kMaxSlot)
            return refuse(BlobError::SlotOutOfRange, who + " is out of range (0 to " + std::to_string(kMaxSlot) + ")");
        if (seen[s.slot])
            return refuse(BlobError::SlotOrder, who + " appears twice");
        seen[s.slot] = true;

        if (s.rootNote > kMaxRootNote)
            return refuse(BlobError::BadRootNote, who + " root note " + std::to_string(s.rootNote) +
                                                      " is out of range (0 to " + std::to_string(kMaxRootNote) + ")");
        if (s.channels < 1 || s.channels > kMaxChannels)
            return refuse(BlobError::BadChannelCount, who + " has " + std::to_string(s.channels) +
                                                          " channels (1 or 2 allowed)");
        if (s.name.size() > kMaxNameBytes)
            return refuse(BlobError::NameTooLong, who + " name is longer than " + std::to_string(kMaxNameBytes) + " bytes");
        if (s.samples.size() % static_cast<std::size_t>(s.channels) != 0)
            return refuse(BlobError::BadSampleCount, who + " sample count is not a multiple of its channel count");
        const std::size_t frames = s.frames();
        if (frames == 0)
            return refuse(BlobError::EmptySample, who + " has no frames");
        if (frames > kMaxSampleFrames)
            return refuse(BlobError::SampleTooLong, who + " is longer than " + std::to_string(kMaxSampleSeconds) + " s");
        if (!isValidUtf8(reinterpret_cast<const std::uint8_t*>(s.name.data()), s.name.size()))
            return refuse(BlobError::BadName, who + " name is not valid UTF-8");
        for (float f : s.samples)
            if (!floatIsFinite(f))
                return refuse(BlobError::NotFinite, who + " has a sample that is not a finite number");
    }

    const std::size_t total = sampleBlobSize(slots);
    if (total > kMaxBlobBytes)
        return setTooLarge(total);
    return {};
}

BlobResult encodeSampleBlob(const SampleSlotSet& slots, std::vector<std::uint8_t>& out)
{
    out.clear();
    const BlobResult r = checkSampleSlots(slots);
    if (!r.ok())
        return r;

    std::vector<std::size_t> order(slots.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(),
              [&slots](std::size_t a, std::size_t b) { return slots[a].slot < slots[b].slot; });

    out.reserve(sampleBlobSize(slots));
    out.insert(out.end(), kBlobHeader, kBlobHeader + kBlobHeaderBytes);
    putU32(out, kSampleRateHz);
    putU32(out, static_cast<std::uint32_t>(slots.size()));
    for (std::size_t idx : order)
    {
        const SampleSlot& s = slots[idx];
        out.push_back(static_cast<std::uint8_t>(s.slot));
        out.push_back(s.rootNote);
        out.push_back(static_cast<std::uint8_t>(s.channels));
        out.push_back(static_cast<std::uint8_t>(s.name.size()));
        putU32(out, static_cast<std::uint32_t>(s.frames()));
        out.insert(out.end(), s.name.begin(), s.name.end());
        for (float f : s.samples)
            putFloat(out, f);
    }
    return {};
}

bool looksLikeSampleBlob(const std::uint8_t* data, std::size_t size) noexcept
{
    return data != nullptr && size >= kBlobFamilyPrefixBytes &&
           std::memcmp(data, kBlobFamilyPrefix, kBlobFamilyPrefixBytes) == 0;
}

BlobResult checkSampleBlob(const std::uint8_t* data, std::size_t size)
{
    if (data == nullptr || size < kBlobHeaderBytes || std::memcmp(data, kBlobHeader, kBlobHeaderBytes) != 0)
        return refuse(BlobError::UnknownHeader, "unknown format header");
    if (size > kMaxBlobBytes)
        return setTooLarge(size);
    if (size < kBlobPreambleBytes)
        return refuse(BlobError::Truncated, "truncated blob");

    const std::uint32_t rate = getU32(data + 16);
    if (rate != kSampleRateHz)
        return refuse(BlobError::WrongSampleRate, "sample rate " + std::to_string(rate) + " Hz is not " +
                                                      std::to_string(kSampleRateHz) + " Hz");

    const std::uint32_t count = getU32(data + 20);
    if (count > static_cast<std::uint32_t>(kSlotCount))
        return refuse(BlobError::TooManySlots, "too many slots (" + std::to_string(count) + ")");

    std::size_t pos = kBlobPreambleBytes;
    int previous = kNoSlot;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        // Every length below is compared with the bytes that remain before it is used.
        if (size - pos < kSlotRecordHeadBytes)
            return refuse(BlobError::Truncated, "truncated blob");
        const std::uint8_t* head = data + pos;
        const int slot = head[0];
        const unsigned root = head[1];
        const unsigned channels = head[2];
        const std::size_t nameLen = head[3];
        const std::uint32_t frames = getU32(head + 4);
        const std::string who = slotText(slot);

        if (slot > kMaxSlot)
            return refuse(BlobError::SlotOutOfRange, who + " is out of range (0 to " + std::to_string(kMaxSlot) + ")");
        if (slot <= previous)
            return refuse(BlobError::SlotOrder, who + (slot == previous ? " appears twice" : " is out of order"));
        previous = slot;
        if (root > kMaxRootNote)
            return refuse(BlobError::BadRootNote, who + " root note " + std::to_string(root) +
                                                      " is out of range (0 to " + std::to_string(kMaxRootNote) + ")");
        if (channels < 1 || channels > static_cast<unsigned>(kMaxChannels))
            return refuse(BlobError::BadChannelCount, who + " has " + std::to_string(channels) +
                                                          " channels (1 or 2 allowed)");
        if (nameLen > kMaxNameBytes)
            return refuse(BlobError::NameTooLong, who + " name is longer than " + std::to_string(kMaxNameBytes) + " bytes");
        if (frames == 0)
            return refuse(BlobError::EmptySample, who + " has no frames");
        if (frames > kMaxSampleFrames)
            return refuse(BlobError::SampleTooLong, who + " is longer than " + std::to_string(kMaxSampleSeconds) + " s");

        // frames <= 2 880 000 and channels <= 2, so this cannot overflow.
        const std::size_t sampleBytes = static_cast<std::size_t>(frames) * channels * 4;
        const std::size_t remaining = size - pos - kSlotRecordHeadBytes;
        if (nameLen > remaining || sampleBytes > remaining - nameLen)
            return refuse(BlobError::Truncated, "truncated blob");

        const std::uint8_t* name = head + kSlotRecordHeadBytes;
        const std::uint8_t* samples = name + nameLen;
        if (!isValidUtf8(name, nameLen))
            return refuse(BlobError::BadName, who + " name is not valid UTF-8");
        for (std::size_t b = 0; b < sampleBytes; b += 4)
            if (!bitsAreFinite(getU32(samples + b)))
                return refuse(BlobError::NotFinite, who + " has a sample that is not a finite number");

        pos += kSlotRecordHeadBytes + nameLen + sampleBytes;
    }

    if (pos != size)
        return refuse(BlobError::TrailingBytes, "unexpected bytes after the last slot");
    return {};
}

BlobResult decodeSampleBlob(const std::uint8_t* data, std::size_t size, SampleSlotSet& out)
{
    out.clear();
    const BlobResult r = checkSampleBlob(data, size);
    if (!r.ok())
        return r;

    // The blob is valid, so the fields below are all in range and inside `size`.
    const std::uint32_t count = getU32(data + 20);
    out.reserve(count);
    std::size_t pos = kBlobPreambleBytes;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const std::uint8_t* head = data + pos;
        SampleSlot s;
        s.slot = head[0];
        s.rootNote = head[1];
        s.channels = head[2];
        const std::size_t nameLen = head[3];
        const std::size_t sampleCount = static_cast<std::size_t>(getU32(head + 4)) * static_cast<std::size_t>(s.channels);
        const std::uint8_t* name = head + kSlotRecordHeadBytes;
        const std::uint8_t* samples = name + nameLen;

        s.name.assign(reinterpret_cast<const char*>(name), nameLen);
        s.samples.resize(sampleCount);
        for (std::size_t k = 0; k < sampleCount; ++k)
            s.samples[k] = getFloat(samples + 4 * k);
        out.push_back(std::move(s));
        pos += kSlotRecordHeadBytes + nameLen + 4 * sampleCount;
    }
    return {};
}

std::string sampleBlobRefusal(const std::uint8_t* data, std::size_t size)
{
    if (!looksLikeSampleBlob(data, size))
        return {};
    return checkSampleBlob(data, size).message;
}

std::string sanitiseSampleName(const std::string& name)
{
    const auto* in = reinterpret_cast<const std::uint8_t*>(name.data());
    std::string kept;
    std::size_t i = 0;
    while (i < name.size())
    {
        const std::size_t len = utf8SequenceLength(in + i, name.size() - i);
        if (len == 0)
        {
            ++i; // invalid byte: drop it and look again at the next one
            continue;
        }
        if (len == 1 && (in[i] < 0x20 || in[i] == 0x7F))
        {
            ++i; // control character
            continue;
        }
        kept.append(name, i, len);
        i += len;
    }

    const auto trim = [](std::string& s)
    {
        const std::size_t first = s.find_first_not_of(' ');
        if (first == std::string::npos)
        {
            s.clear();
            return;
        }
        s.erase(0, first);
        s.erase(s.find_last_not_of(' ') + 1);
    };
    trim(kept);

    if (kept.size() > kMaxNameBytes)
    {
        std::size_t cut = kMaxNameBytes;
        // Step back off any continuation bytes so the cut lands on a character boundary.
        while (cut > 0 && (static_cast<std::uint8_t>(kept[cut]) & 0xC0) == 0x80)
            --cut;
        kept.resize(cut);
        trim(kept);
    }
    return kept.empty() ? std::string("sample") : kept;
}

bool applyBlobFaultForTesting(std::vector<std::uint8_t>& blob, BlobFault fault)
{
    const auto putAt = [&blob](std::size_t offset, std::uint32_t v)
    {
        for (std::size_t i = 0; i < 4; ++i)
            blob[offset + i] = static_cast<std::uint8_t>(v >> (8 * i));
    };
    const bool hasSlot = blob.size() >= kBlobPreambleBytes + kSlotRecordHeadBytes && getU32(blob.data() + 20) != 0;

    switch (fault)
    {
    case BlobFault::WrongHeader:
        if (blob.size() < kBlobHeaderBytes)
            return false;
        blob[kBlobHeaderBytes - 1] = static_cast<std::uint8_t>('9');
        return true;
    case BlobFault::WrongSampleRate:
        if (blob.size() < kBlobPreambleBytes)
            return false;
        putAt(16, 44100);
        return true;
    case BlobFault::SlotOutOfRange:
        if (!hasSlot)
            return false;
        blob[kBlobPreambleBytes] = static_cast<std::uint8_t>(kSlotCount);
        return true;
    case BlobFault::SampleTooLong:
        if (!hasSlot)
            return false;
        putAt(kBlobPreambleBytes + 4, kMaxSampleFrames + 1);
        return true;
    case BlobFault::Truncated:
        if (blob.size() < 4)
            return false;
        blob.resize(blob.size() - 4);
        return true;
    }
    return false;
}

} // namespace phylo::samples
