#include "phylo/host/Fnv1a.h"

namespace phylo::host
{

std::uint64_t fnv1a64(const std::uint8_t* data, std::size_t size) noexcept
{
    std::uint64_t h = kFnv1a64Offset;
    for (std::size_t i = 0; i < size; ++i)
    {
        h ^= static_cast<std::uint64_t>(data[i]);
        h *= kFnv1a64Prime; // unsigned: wraps modulo 2^64 by definition
    }
    return h;
}

std::string hex16(std::uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i)
    {
        out[static_cast<std::size_t>(i)] = digits[value & 0xFu];
        value >>= 4;
    }
    return out;
}

std::string fnv1a64Hex(const std::uint8_t* data, std::size_t size)
{
    return hex16(fnv1a64(data, size));
}

} // namespace phylo::host
