// FNV-1a, 64 bit. JUCE-free.
//
// Used to tell a plugin which sample set it was sent: the plugin hashes the blob it received
// and the host compares (docs/SAMPLE_INTERFACE.md, "The answer"). Start 0xcbf29ce484222325;
// for each byte: xor the byte, then multiply by 0x100000001b3, modulo 2^64.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace phylo::host
{

constexpr std::uint64_t kFnv1a64Offset = 0xcbf29ce484222325ull;
constexpr std::uint64_t kFnv1a64Prime = 0x100000001b3ull;

// Hash of `size` bytes. `data` may be null when `size` is 0.
std::uint64_t fnv1a64(const std::uint8_t* data, std::size_t size) noexcept;

// The hash as exactly 16 lowercase hex digits, zero padded.
std::string fnv1a64Hex(const std::uint8_t* data, std::size_t size);

// The same formatting for a value that is already hashed.
std::string hex16(std::uint64_t value);

} // namespace phylo::host
