#include "phylo/host/Vst3Preset.h"

#include <algorithm>
#include <cstring>

namespace phylo::host
{
namespace
{

PresetResult fail(PresetError code, const char* message)
{
    PresetResult r;
    r.code = code;
    r.message = message;
    return r;
}

std::uint32_t readU32(const std::uint8_t* p) noexcept
{
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t readU64(const std::uint8_t* p) noexcept
{
    return static_cast<std::uint64_t>(readU32(p)) | (static_cast<std::uint64_t>(readU32(p + 4)) << 32);
}

void putU32(std::vector<std::uint8_t>& v, std::uint32_t x)
{
    for (int i = 0; i < 4; ++i)
        v.push_back(static_cast<std::uint8_t>((x >> (8 * i)) & 0xFFu));
}

void putU64(std::vector<std::uint8_t>& v, std::uint64_t x)
{
    for (int i = 0; i < 8; ++i)
        v.push_back(static_cast<std::uint8_t>((x >> (8 * i)) & 0xFFu));
}

bool isHexDigit(unsigned char c) noexcept
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool validClassId(const std::string& id) noexcept
{
    if (id.size() != kVst3ClassIdChars)
        return false;
    return std::all_of(id.begin(), id.end(), [](char c) { return isHexDigit(static_cast<unsigned char>(c)); });
}

} // namespace

PresetResult parseVst3Preset(const std::uint8_t* data, std::size_t sizeIn, Vst3Preset& out)
{
    out = Vst3Preset{};
    const std::uint64_t size = sizeIn;

    if (data == nullptr || size < kVst3HeaderBytes)
        return fail(PresetError::TooSmall, "preset container is smaller than its header");
    if (std::memcmp(data, "VST3", 4) != 0)
        return fail(PresetError::BadMagic, "preset container does not start with VST3");
    if (static_cast<std::int32_t>(readU32(data + 4)) != kVst3FormatVersion)
        return fail(PresetError::BadVersion, "preset container version is not 1");

    Vst3Preset result;
    result.classId.assign(reinterpret_cast<const char*>(data + 8), kVst3ClassIdChars);
    if (!validClassId(result.classId))
        return fail(PresetError::BadClassId, "preset class ID is not 32 hex digits");

    // The list lies after the header and must have room for its tag and count. Both sides
    // are compared as unsigned 64-bit values after the subtraction, so nothing can wrap.
    const std::uint64_t listOffset = readU64(data + 40);
    if (listOffset < kVst3HeaderBytes || listOffset > size)
        return fail(PresetError::BadListOffset, "preset chunk list offset is outside the container");
    if (size - listOffset < kVst3ListHeaderBytes)
        return fail(PresetError::BadListHeader, "preset chunk list is cut off");

    const std::uint8_t* list = data + static_cast<std::size_t>(listOffset);
    if (std::memcmp(list, "List", 4) != 0)
        return fail(PresetError::BadListHeader, "preset chunk list does not start with List");

    const std::int32_t count = static_cast<std::int32_t>(readU32(list + 4));
    if (count < 0 || count > kVst3MaxChunks)
        return fail(PresetError::BadChunkCount, "preset chunk count is out of range");

    const std::uint64_t room = size - listOffset - kVst3ListHeaderBytes;
    if (static_cast<std::uint64_t>(count) * kVst3ListEntryBytes > room) // at most 128 * 20: no overflow
        return fail(PresetError::ListTruncated, "preset chunk list is cut off");

    const std::uint8_t* entry = list + kVst3ListHeaderBytes;
    for (std::int32_t i = 0; i < count; ++i, entry += kVst3ListEntryBytes)
    {
        Vst3Chunk c;
        c.id.assign(reinterpret_cast<const char*>(entry), 4);
        c.offset = readU64(entry + 4);
        c.size = readU64(entry + 12);
        // Data lies between the header and the end. offset <= size first, then size against
        // what is left, so offset + size is never formed.
        if (c.offset < kVst3HeaderBytes || c.offset > size || c.size > size - c.offset)
            return fail(PresetError::BadChunkRange, "preset chunk lies outside the container");
        result.chunks.push_back(std::move(c));
    }

    out = std::move(result);
    return {};
}

PresetResult readVst3Chunk(const std::uint8_t* data, std::size_t size, const char* chunkId,
                           std::string& classId, std::vector<std::uint8_t>& bytes)
{
    classId.clear();
    bytes.clear();

    Vst3Preset preset;
    const PresetResult parsed = parseVst3Preset(data, size, preset);
    if (!parsed.ok())
        return parsed;

    if (chunkId == nullptr || std::strlen(chunkId) != 4)
        return fail(PresetError::BadChunkId, "chunk ID must be 4 characters");

    for (const Vst3Chunk& c : preset.chunks)
    {
        if (c.id.compare(0, 4, chunkId, 4) != 0)
            continue;
        // parseVst3Preset has checked that the range is inside the container, which fits size_t.
        const std::uint8_t* from = data + static_cast<std::size_t>(c.offset);
        bytes.assign(from, from + static_cast<std::size_t>(c.size));
        classId = preset.classId;
        return {};
    }
    return fail(PresetError::ChunkNotFound, "preset has no such chunk");
}

PresetResult buildVst3Preset(const std::string& classId, const std::uint8_t* comp, std::size_t compSize,
                             std::vector<std::uint8_t>& out)
{
    out.clear();
    if (!validClassId(classId))
        return fail(PresetError::BadClassId, "preset class ID is not 32 hex digits");
    if (compSize > 0 && comp == nullptr)
        return fail(PresetError::BadChunkRange, "component state is missing");

    const std::uint64_t listBytes = kVst3ListHeaderBytes + kVst3ListEntryBytes; // one entry
    const std::uint64_t compBytes = compSize;
    if (compBytes > kVst3MaxContainerBytes || kVst3HeaderBytes + compBytes + listBytes > kVst3MaxContainerBytes)
        return fail(PresetError::TooLarge, "component state is too large for a preset container");

    const std::uint64_t listOffset = kVst3HeaderBytes + compBytes;
    std::vector<std::uint8_t> v;
    v.reserve(static_cast<std::size_t>(listOffset + listBytes));

    static const char magic[4] = {'V', 'S', 'T', '3'};
    v.insert(v.end(), magic, magic + 4);
    putU32(v, static_cast<std::uint32_t>(kVst3FormatVersion));
    v.insert(v.end(), classId.begin(), classId.end());
    putU64(v, listOffset);
    if (compSize > 0)
        v.insert(v.end(), comp, comp + compSize);

    static const char list[4] = {'L', 'i', 's', 't'};
    static const char compId[4] = {'C', 'o', 'm', 'p'};
    v.insert(v.end(), list, list + 4);
    putU32(v, 1);
    v.insert(v.end(), compId, compId + 4);
    putU64(v, kVst3HeaderBytes);
    putU64(v, compBytes);

    out = std::move(v);
    return {};
}

} // namespace phylo::host
