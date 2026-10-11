// The VST3 preset container (".vstpreset" layout). JUCE-free.
//
// JUCE's VST3 host exposes a plugin's component state only as a whole preset container
// (ExtensionsVisitor::VST3Client::getPreset / setPreset). Phylotone uses it to hand the
// plugin a sample blob as its component state (docs/SAMPLE_INTERFACE.md, "Delivery"), and to
// read the plugin's state back. The layout below was read from the VST3 SDK's
// vstpresetfile.cpp, as shipped with JUCE 8.0.9.
//
// All integers are little-endian (the SDK byte-swaps on big-endian hosts).
//
//   offset  size  field
//   0       4     "VST3"
//   4       4     i32 format version, 1
//   8       32    class ID, 32 ASCII hex digits (a FUID as text)
//   40      8     i64 offset of the chunk list
//   48      ...   chunk data, back to back
//   L       4     "List" (at the chunk-list offset)
//   L+4     4     i32 entry count
//   L+8     20*n  entries: 4-byte chunk ID, i64 data offset, i64 data size
//
// Chunk IDs used here: "Comp" (component state), "Cont" (controller state), "Info" (meta).
//
// The reader treats the container as hostile: every offset and size is checked against the
// bytes that are really there, in 64-bit arithmetic that cannot wrap, before it is used.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace phylo::host
{

constexpr std::size_t kVst3ClassIdChars = 32;
constexpr std::size_t kVst3HeaderBytes = 48;       // "VST3", version, class ID, list offset
constexpr std::size_t kVst3ListHeaderBytes = 8;    // "List", entry count
constexpr std::size_t kVst3ListEntryBytes = 20;    // ID, offset, size
constexpr std::int32_t kVst3FormatVersion = 1;
constexpr std::int32_t kVst3MaxChunks = 128;       // the SDK reads at most this many entries
constexpr std::size_t kVst3MaxContainerBytes = 0x7fffffffu; // JUCE hands the container over as an int-sized stream

enum class PresetError
{
    None,
    TooSmall,        // fewer bytes than the header
    BadMagic,        // does not start with "VST3"
    BadVersion,      // version is not 1
    BadClassId,      // class ID is not 32 hex digits
    BadListOffset,   // list offset before the data area or past the end
    BadListHeader,   // no "List" tag, or no room for the entry count
    BadChunkCount,   // negative, or above kVst3MaxChunks
    ListTruncated,   // the entries do not fit in the container
    BadChunkRange,   // a chunk's data lies outside the container
    ChunkNotFound,   // no chunk with the requested ID
    BadChunkId,      // build: a chunk ID that is not 4 bytes
    TooLarge         // build: the container would not fit kVst3MaxContainerBytes
};

struct PresetResult
{
    PresetError code = PresetError::None;
    std::string message;
    bool ok() const noexcept { return code == PresetError::None; }
};

struct Vst3Chunk
{
    std::string id;           // 4 bytes
    std::uint64_t offset = 0; // from the start of the container
    std::uint64_t size = 0;
};

struct Vst3Preset
{
    std::string classId;            // 32 hex digits, as stored
    std::vector<Vst3Chunk> chunks;  // in list order; every range is inside the container
};

// Parses the header and chunk list. On a refusal `out` is left empty.
PresetResult parseVst3Preset(const std::uint8_t* data, std::size_t size, Vst3Preset& out);

// Parses, then copies the bytes of the first chunk named `chunkId` (4 characters) into
// `bytes`. `classId` receives the container's class ID. On a refusal both are left empty.
PresetResult readVst3Chunk(const std::uint8_t* data, std::size_t size, const char* chunkId,
                           std::string& classId, std::vector<std::uint8_t>& bytes);

// Builds a container with one "Comp" chunk holding `comp`. `classId` must be 32 hex digits.
// On a refusal `out` is left empty.
PresetResult buildVst3Preset(const std::string& classId, const std::uint8_t* comp, std::size_t compSize,
                             std::vector<std::uint8_t>& out);

// Size of the container buildVst3Preset writes for a component state of `compSize` bytes, or
// 0 when it would not fit kVst3MaxContainerBytes.
std::size_t vst3PresetSize(std::size_t compSize) noexcept;

// The same container, written straight into caller memory of exactly vst3PresetSize(compSize)
// bytes, so a large state need not be built in a vector and copied. `comp` may be null only
// when compSize is 0. `dest` and `comp` must not overlap.
PresetResult writeVst3Preset(const std::string& classId, const std::uint8_t* comp, std::size_t compSize,
                             std::uint8_t* dest, std::size_t destSize);

} // namespace phylo::host
