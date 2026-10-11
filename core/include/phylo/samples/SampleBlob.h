// The `akazi.samples.v1` state blob: the full slot set Phylotone sends to Akazi XL over
// SetState (docs/SAMPLE_INTERFACE.md). JUCE-free.
//
// Layout. All integers are little-endian, floats are IEEE 754 bit patterns.
//
//   offset  size  field
//   0       16    header, the ASCII bytes "akazi.samples.v1" (no terminator)
//   16      4     u32 sample rate in Hz, always 48000
//   20      4     u32 slot count N, 0 to 128
//   24      ...   N slot records, in strictly ascending slot order
//
// One slot record:
//
//   0       1     u8 slot number, 0 to 127
//   1       1     u8 root note (MIDI note the sample plays at its own pitch), 0 to 127
//   2       1     u8 channel count, 1 or 2
//   3       1     u8 name length L in bytes, 0 to 64
//   4       4     u32 frame count F, 1 to 2 880 000 (60 s at 48 kHz)
//   8       L     name, UTF-8, no terminator
//   8 + L   4*F*C float32 samples, interleaved (C = channel count)
//
// The blob ends with the last slot record: bytes after it are refused. The blob is a full
// replacement, so a slot that is not in it is cleared. A blob with N = 0 clears every slot.
//
// The decoder checks, in this order, and refuses with the first problem it finds:
// header, sample rate, slot count, then per slot: slot range, slot order, root note,
// channel count, name length, frame count (zero, then over the limit), truncation (name
// and sample bytes present), name is UTF-8, samples are finite. Last: trailing bytes.
#pragma once

#include "phylo/host/Wire.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace phylo::samples
{

constexpr char kBlobHeader[] = "akazi.samples.v1";      // 16 bytes on the wire
constexpr std::size_t kBlobHeaderBytes = 16;
constexpr char kBlobFamilyPrefix[] = "akazi.samples.";  // any version of this format
constexpr std::size_t kBlobFamilyPrefixBytes = 14;
constexpr std::size_t kBlobPreambleBytes = 24;          // header, sample rate, slot count
constexpr std::size_t kSlotRecordHeadBytes = 8;

constexpr std::uint32_t kSampleRateHz = 48000;
constexpr int kSlotCount = 128;
constexpr int kMaxSlot = kSlotCount - 1;
constexpr int kNoSlot = -1;
constexpr int kMaxChannels = 2;
constexpr std::size_t kMaxNameBytes = 64;
constexpr std::uint32_t kMaxSampleSeconds = 60;
constexpr std::uint32_t kMaxSampleFrames = kMaxSampleSeconds * kSampleRateHz;

// OPEN DECISION (Thomas): root note per slot. Proposed: optional, default middle C. The
// blob always carries one byte for it; a slot whose root was never set carries this value.
constexpr std::uint8_t kDefaultRootNote = 60;
constexpr std::uint8_t kMaxRootNote = 127;

// OPEN DECISION (Thomas, needs an Akazi XL memory check): the budget for the whole slot
// set. Version 1 sends the set as one SetState frame, so it cannot be larger than one wire
// payload. One 60 s stereo sample is about 23 MB.
constexpr std::size_t kMaxBlobBytes = phylo::host::kMaxPayloadBytes;

enum class BlobError
{
    None,
    UnknownHeader,     // not "akazi.samples.v1"
    WrongSampleRate,   // not 48 kHz
    TooManySlots,      // slot count above 128
    SlotOutOfRange,    // slot number above 127 (or below 0 when encoding)
    SlotOrder,         // a slot twice, or (decoding) not in ascending order
    BadRootNote,       // above 127
    BadChannelCount,   // not 1 or 2
    NameTooLong,       // more than 64 bytes
    EmptySample,       // zero frames: an empty slot is left out of the blob, not sent
    SampleTooLong,     // more than 60 s
    Truncated,         // the blob ends before the data it describes
    BadName,           // name is not valid UTF-8
    NotFinite,         // a sample is NaN or infinite
    TrailingBytes,     // bytes after the last slot record
    SetTooLarge,       // the whole blob is over kMaxBlobBytes
    BadSampleCount     // encoding only: samples.size() is not a multiple of the channel count
};

// The outcome of a check. `message` is the refusal reason for the status line, written so
// it reads on its own, for example "slot 128 is out of range (0 to 127)".
struct BlobResult
{
    BlobError code = BlobError::None;
    std::string message;

    bool ok() const noexcept { return code == BlobError::None; }
};

// One non-empty slot.
struct SampleSlot
{
    int slot = 0;                             // 0 to 127
    std::string name;                         // UTF-8, at most 64 bytes
    std::uint8_t rootNote = kDefaultRootNote; // 0 to 127
    int channels = 1;                         // 1 or 2
    std::vector<float> samples;               // interleaved, 48 kHz, frames * channels

    std::size_t frames() const noexcept
    {
        return channels > 0 ? samples.size() / static_cast<std::size_t>(channels) : 0;
    }
};

using SampleSlotSet = std::vector<SampleSlot>;

// Exact size in bytes of the blob encodeSampleBlob would write for these slots. Does not
// validate; a slot with bad fields is counted as it stands.
std::size_t sampleBlobSize(const SampleSlotSet& slots);

// Checks a slot set against every rule the decoder enforces, plus the set budget. Slots
// may be in any order.
BlobResult checkSampleSlots(const SampleSlotSet& slots);

// Encodes the set, slots sorted ascending. On a refusal `out` is left empty and nothing is
// allocated for the sample data.
BlobResult encodeSampleBlob(const SampleSlotSet& slots, std::vector<std::uint8_t>& out);

// True when the bytes start with "akazi.samples.", whatever the version. Used to tell a
// sample blob (to be checked) from a plugin's own state (passed through untouched).
bool looksLikeSampleBlob(const std::uint8_t* data, std::size_t size) noexcept;

// Validates a blob without copying the sample data. Safe on hostile input: every length is
// checked against the bytes that are really there before it is used.
BlobResult checkSampleBlob(const std::uint8_t* data, std::size_t size);

// Validates, then fills `out`. On a refusal `out` is left empty.
BlobResult decodeSampleBlob(const std::uint8_t* data, std::size_t size, SampleSlotSet& out);

// What a receiver of SetState bytes answers in StateResult: the refusal reason when the
// bytes are a sample blob that fails checkSampleBlob, otherwise an empty string (a valid
// blob, or bytes that are not a sample blob at all).
std::string sampleBlobRefusal(const std::uint8_t* data, std::size_t size);

// Makes a name safe to store and send: invalid UTF-8 and control characters are dropped,
// surrounding whitespace is trimmed, and the result is cut to at most kMaxNameBytes at a
// character boundary. An empty result becomes "sample".
std::string sanitiseSampleName(const std::string& name);

// Test hook: damages a valid blob in place so the refusal path can be exercised from the
// app and the headless checks. Each fault produces the BlobError of the same meaning.
enum class BlobFault
{
    WrongHeader,     // "akazi.samples.v9"                    -> UnknownHeader
    WrongSampleRate, // sample rate field set to 44100        -> WrongSampleRate
    SlotOutOfRange,  // first slot's number set to 128        -> SlotOutOfRange
    SampleTooLong,   // first slot's frame count set to 61 s  -> SampleTooLong
    Truncated        // the last four bytes removed           -> Truncated
};

// Returns false, leaving the blob untouched, when the fault needs a slot and there is none.
bool applyBlobFaultForTesting(std::vector<std::uint8_t>& blob, BlobFault fault);

} // namespace phylo::samples
