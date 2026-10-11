// Tests for the pieces of sample delivery that need no JUCE: the VST3 preset container
// (core/include/phylo/host/Vst3Preset.h) and FNV-1a 64 (core/include/phylo/host/Fnv1a.h).

#include "check.h"

#include "phylo/host/Fnv1a.h"
#include "phylo/host/Vst3Preset.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace phylo::host;

namespace
{

using Bytes = std::vector<std::uint8_t>;

const char kClass[] = "0123456789ABCDEFfedcba9876543210"; // 32 hex digits, mixed case

Bytes bytesOf(const std::string& s)
{
    return Bytes(s.begin(), s.end());
}

std::uint64_t hashOf(const std::string& s)
{
    return fnv1a64(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

void put32(Bytes& v, std::size_t at, std::uint32_t x)
{
    for (int i = 0; i < 4; ++i)
        v[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((x >> (8 * i)) & 0xFFu);
}

void put64(Bytes& v, std::size_t at, std::uint64_t x)
{
    for (int i = 0; i < 8; ++i)
        v[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((x >> (8 * i)) & 0xFFu);
}

std::uint32_t get32(const Bytes& v, std::size_t at)
{
    std::uint32_t x = 0;
    for (int i = 0; i < 4; ++i)
        x |= static_cast<std::uint32_t>(v[at + static_cast<std::size_t>(i)]) << (8 * i);
    return x;
}

std::uint64_t get64(const Bytes& v, std::size_t at)
{
    return static_cast<std::uint64_t>(get32(v, at)) | (static_cast<std::uint64_t>(get32(v, at + 4)) << 32);
}

Bytes valid(const Bytes& comp = bytesOf("component state"))
{
    Bytes out;
    CHECK(buildVst3Preset(kClass, comp.data(), comp.size(), out).ok());
    return out;
}

// A container the way the SDK writes one with a plugin that has a controller: Comp, Cont,
// Info, then the list. Written by hand from the layout, not with buildVst3Preset.
Bytes sdkStyle(const Bytes& comp, const Bytes& cont, const std::string& info)
{
    Bytes v(48, 0);
    std::memcpy(v.data(), "VST3", 4);
    put32(v, 4, 1);
    std::memcpy(v.data() + 8, kClass, 32);
    const std::uint64_t compAt = v.size();
    v.insert(v.end(), comp.begin(), comp.end());
    const std::uint64_t contAt = v.size();
    v.insert(v.end(), cont.begin(), cont.end());
    const std::uint64_t infoAt = v.size();
    v.insert(v.end(), info.begin(), info.end());
    const std::uint64_t listAt = v.size();
    put64(v, 40, listAt);
    const auto entry = [&v](const char* id, std::uint64_t off, std::uint64_t size)
    {
        v.insert(v.end(), id, id + 4);
        Bytes n(16, 0);
        put64(n, 0, off);
        put64(n, 8, size);
        v.insert(v.end(), n.begin(), n.end());
    };
    v.insert(v.end(), {'L', 'i', 's', 't', 3, 0, 0, 0});
    entry("Comp", compAt, comp.size());
    entry("Cont", contAt, cont.size());
    entry("Info", infoAt, info.size());
    return v;
}

PresetResult readComp(const Bytes& c, std::string& cls, Bytes& out)
{
    return readVst3Chunk(c.data(), c.size(), "Comp", cls, out);
}

bool refuses(const Bytes& c, PresetError want)
{
    std::string cls;
    Bytes out;
    const PresetResult r = readComp(c, cls, out);
    return r.code == want && !r.message.empty() && cls.empty() && out.empty();
}

} // namespace

TEST(fnv_known_vectors)
{
    CHECK_EQ(fnv1a64(nullptr, 0), 0xcbf29ce484222325ull);
    CHECK_EQ(hashOf(""), 0xcbf29ce484222325ull);
    CHECK_EQ(hashOf("a"), 0xaf63dc4c8601ec8cull);
    CHECK_EQ(hashOf("foobar"), 0x85944171f73967e8ull);
    CHECK(fnv1a64Hex(nullptr, 0) == "cbf29ce484222325");
    CHECK(fnv1a64Hex(reinterpret_cast<const std::uint8_t*>("a"), 1) == "af63dc4c8601ec8c");
}

TEST(fnv_hex_is_sixteen_lowercase_digits)
{
    CHECK(hex16(0) == "0000000000000000");
    CHECK(hex16(1) == "0000000000000001");
    CHECK(hex16(0xFFFFFFFFFFFFFFFFull) == "ffffffffffffffff");
    CHECK(hex16(0x0123456789abcdefull) == "0123456789abcdef");
}

TEST(fnv_depends_on_every_byte_and_their_order)
{
    Bytes a(1000);
    for (std::size_t i = 0; i < a.size(); ++i)
        a[i] = static_cast<std::uint8_t>(i * 7);
    const std::uint64_t h = fnv1a64(a.data(), a.size());
    for (std::size_t i = 0; i < a.size(); i += 37)
    {
        Bytes b = a;
        b[i] ^= 1;
        CHECK(fnv1a64(b.data(), b.size()) != h);
    }
    Bytes swapped = a;
    std::swap(swapped[10], swapped[11]);
    CHECK(fnv1a64(swapped.data(), swapped.size()) != h);
    CHECK(fnv1a64(a.data(), a.size() - 1) != h);
}

TEST(preset_round_trip_and_exact_layout)
{
    const Bytes comp = bytesOf("akazi.samples.v1 and some more bytes");
    const Bytes c = valid(comp);

    // Layout: header 48, data, "List", count, one 20-byte entry.
    CHECK_EQ(c.size(), 48u + comp.size() + 8u + 20u);
    CHECK(std::memcmp(c.data(), "VST3", 4) == 0);
    CHECK_EQ(get32(c, 4), 1u);
    CHECK(std::memcmp(c.data() + 8, kClass, 32) == 0);
    CHECK_EQ(get64(c, 40), 48u + comp.size());
    CHECK(std::memcmp(c.data() + 48, comp.data(), comp.size()) == 0);
    const std::size_t list = 48 + comp.size();
    CHECK(std::memcmp(c.data() + list, "List", 4) == 0);
    CHECK_EQ(get32(c, list + 4), 1u);
    CHECK(std::memcmp(c.data() + list + 8, "Comp", 4) == 0);
    CHECK_EQ(get64(c, list + 12), 48u);
    CHECK_EQ(get64(c, list + 20), comp.size());

    std::string cls;
    Bytes out;
    CHECK(readComp(c, cls, out).ok());
    CHECK(cls == kClass);
    CHECK(out == comp);
}

TEST(preset_empty_component_state)
{
    const Bytes c = valid(Bytes{});
    CHECK_EQ(c.size(), 48u + 28u);
    std::string cls;
    Bytes out;
    CHECK(readComp(c, cls, out).ok());
    CHECK(out.empty());
    CHECK(cls == kClass);
}

TEST(preset_reads_an_sdk_style_container)
{
    const Bytes comp = bytesOf("<xml/>");
    const Bytes cont = bytesOf("controller bytes");
    const Bytes c = sdkStyle(comp, cont, "<MetaInfo/>");

    Vst3Preset p;
    CHECK(parseVst3Preset(c.data(), c.size(), p).ok());
    CHECK_EQ(p.chunks.size(), 3u);
    CHECK(p.classId == kClass);

    std::string cls;
    Bytes out;
    CHECK(readVst3Chunk(c.data(), c.size(), "Comp", cls, out).ok());
    CHECK(out == comp);
    CHECK(readVst3Chunk(c.data(), c.size(), "Cont", cls, out).ok());
    CHECK(out == cont);
    CHECK(readVst3Chunk(c.data(), c.size(), "Info", cls, out).ok());
    CHECK(out == bytesOf("<MetaInfo/>"));
    CHECK(readVst3Chunk(c.data(), c.size(), "Prog", cls, out).code == PresetError::ChunkNotFound);
    CHECK(out.empty() && cls.empty());
}

TEST(preset_first_chunk_of_a_name_wins)
{
    Bytes c = sdkStyle(bytesOf("first"), bytesOf("second"), "x");
    // Rename the Cont entry to Comp.
    const std::size_t list = static_cast<std::size_t>(get64(c, 40));
    std::memcpy(c.data() + list + 8 + 20, "Comp", 4);
    std::string cls;
    Bytes out;
    CHECK(readComp(c, cls, out).ok());
    CHECK(out == bytesOf("first"));
}

TEST(preset_every_truncation_is_refused)
{
    const Bytes c = valid(bytesOf("some state"));
    for (std::size_t n = 0; n < c.size(); ++n)
    {
        std::string cls;
        Bytes out;
        const PresetResult r = readVst3Chunk(c.data(), n, "Comp", cls, out);
        CHECK(!r.ok());
        CHECK(!r.message.empty());
        CHECK(out.empty());
    }
    std::string cls;
    Bytes out;
    CHECK(readVst3Chunk(nullptr, 0, "Comp", cls, out).code == PresetError::TooSmall);
}

TEST(preset_header_faults)
{
    {
        Bytes c = valid();
        c[0] = 'X';
        CHECK(refuses(c, PresetError::BadMagic));
    }
    {
        Bytes c = valid();
        put32(c, 4, 2);
        CHECK(refuses(c, PresetError::BadVersion));
    }
    {
        Bytes c = valid();
        put32(c, 4, 0xFFFFFFFFu);
        CHECK(refuses(c, PresetError::BadVersion));
    }
    {
        Bytes c = valid();
        c[8 + 5] = 'G'; // not a hex digit
        CHECK(refuses(c, PresetError::BadClassId));
    }
    {
        Bytes c = valid();
        c[8 + 31] = 0; // a NUL inside the class ID
        CHECK(refuses(c, PresetError::BadClassId));
    }
}

TEST(preset_list_offset_faults)
{
    const std::size_t size = valid().size();
    const std::uint64_t cases[] = {0,
                                   47,
                                   static_cast<std::uint64_t>(size),      // at the end: no room for "List"
                                   static_cast<std::uint64_t>(size) - 7,  // room for tag but not the count
                                   static_cast<std::uint64_t>(size) + 1,
                                   0x7fffffffffffffffull,
                                   0x8000000000000000ull,
                                   0xFFFFFFFFFFFFFFF8ull,
                                   0xFFFFFFFFFFFFFFFFull};
    for (std::uint64_t at : cases)
    {
        Bytes c = valid();
        put64(c, 40, at);
        std::string cls;
        Bytes out;
        const PresetResult r = readComp(c, cls, out);
        CHECK(!r.ok());
        CHECK(out.empty());
    }
    // A list offset that points at something that is not "List".
    Bytes c = valid();
    put64(c, 40, 48); // the state bytes, not a list
    CHECK(refuses(c, PresetError::BadListHeader));
    put64(c, 40, 8); // inside the header
    CHECK(refuses(c, PresetError::BadListOffset));
}

TEST(preset_chunk_count_faults)
{
    const std::size_t list = 48 + std::strlen("component state");
    const std::uint32_t cases[] = {0xFFFFFFFFu, 0x80000000u, 0x7fffffffu, 129u, 2u};
    for (std::uint32_t n : cases)
    {
        Bytes c = valid();
        put32(c, list + 4, n);
        std::string cls;
        Bytes out;
        const PresetResult r = readComp(c, cls, out);
        CHECK(!r.ok());
        CHECK(out.empty());
    }
    {
        Bytes c = valid();
        put32(c, list + 4, 0xFFFFFFFFu);
        CHECK(refuses(c, PresetError::BadChunkCount));
    }
    {
        Bytes c = valid();
        put32(c, list + 4, 2); // one entry too many for the bytes there
        CHECK(refuses(c, PresetError::ListTruncated));
    }
    {
        Bytes c = valid();
        put32(c, list + 4, 0); // an empty list has no Comp
        CHECK(refuses(c, PresetError::ChunkNotFound));
    }
    {
        Bytes c = valid();
        c[list] = 'l'; // "list"
        CHECK(refuses(c, PresetError::BadListHeader));
    }
}

TEST(preset_chunk_range_faults_cannot_wrap)
{
    const std::size_t comp = std::strlen("component state");
    const std::size_t entry = 48 + comp + 8; // the first list entry
    struct Case { std::uint64_t offset, size; };
    const Case cases[] = {
        {0, 4},                                         // inside the header
        {47, 1},
        {48, comp + 1},                                 // inside the container: handled below
        {48, 0xFFFFFFFFFFFFFFFFull},
        {0xFFFFFFFFFFFFFFFFull, 1},
        {0x8000000000000000ull, 0x8000000000000000ull}, // offset + size wraps to 0
        {0xFFFFFFFFFFFFFFF0ull, 0x20},                  // wraps to a small number
        {48 + comp + 28 + 1, 0},                        // starts past the end
        {48 + comp + 28, 1},                            // starts at the end, one byte long
        {1, 0xFFFFFFFFFFFFFFFFull},
    };
    for (const Case& k : cases)
    {
        Bytes c = valid();
        put64(c, entry + 4, k.offset);
        put64(c, entry + 12, k.size);
        std::string cls;
        Bytes out;
        const PresetResult r = readComp(c, cls, out);
        // The one case that stays inside the container ({48, comp + 1}) reads the byte of "List".
        if (k.offset == 48 && k.size == comp + 1)
        {
            CHECK(r.ok());
            CHECK_EQ(out.size(), comp + 1);
            continue;
        }
        CHECK(r.code == PresetError::BadChunkRange);
        CHECK(out.empty() && cls.empty());
    }
    // A zero-length chunk exactly at the end is legal.
    {
        Bytes c = valid();
        put64(c, entry + 4, c.size());
        put64(c, entry + 12, 0);
        std::string cls;
        Bytes out;
        CHECK(readComp(c, cls, out).ok());
        CHECK(out.empty());
    }
}

TEST(preset_every_single_byte_change_is_safe)
{
    const Bytes base = sdkStyle(bytesOf("component"), bytesOf("ctl"), "<i/>");
    const std::uint8_t values[] = {0x00, 0x01, 0x7f, 0x80, 0xff};
    for (std::size_t i = 0; i < base.size(); ++i)
    {
        for (std::uint8_t v : values)
        {
            Bytes c = base;
            c[i] = v;
            std::string cls;
            Bytes out;
            const PresetResult r = readComp(c, cls, out);
            if (r.ok())
            {
                CHECK(out.size() <= c.size());
            }
            else
            {
                CHECK(out.empty());
                CHECK(!r.message.empty());
            }
        }
    }
}

TEST(preset_random_bytes_are_safe)
{
    std::uint32_t s = 12345u;
    const auto next = [&s]
    {
        s = s * 1664525u + 1013904223u;
        return static_cast<std::uint8_t>(s >> 24);
    };
    for (int round = 0; round < 2000; ++round)
    {
        Bytes c(static_cast<std::size_t>(round % 200));
        for (auto& b : c)
            b = next();
        if (c.size() >= 4 && round % 2 == 0)
            std::memcpy(c.data(), "VST3", 4);
        std::string cls;
        Bytes out;
        const PresetResult r = readComp(c, cls, out);
        if (!r.ok())
            CHECK(out.empty());
    }
}

TEST(preset_build_refusals)
{
    Bytes out;
    const Bytes comp = bytesOf("x");
    CHECK(buildVst3Preset("", comp.data(), comp.size(), out).code == PresetError::BadClassId);
    CHECK(buildVst3Preset("0123", comp.data(), comp.size(), out).code == PresetError::BadClassId);
    CHECK(buildVst3Preset(std::string(33, 'A'), comp.data(), comp.size(), out).code == PresetError::BadClassId);
    CHECK(buildVst3Preset(std::string(32, 'Z'), comp.data(), comp.size(), out).code == PresetError::BadClassId);
    CHECK(out.empty());
    CHECK(buildVst3Preset(kClass, nullptr, 5, out).code == PresetError::BadChunkRange);
    CHECK(buildVst3Preset(kClass, comp.data(), kVst3MaxContainerBytes, out).code == PresetError::TooLarge);
    CHECK(buildVst3Preset(kClass, comp.data(), static_cast<std::size_t>(0x7fffffffu - 70u), out).code ==
          PresetError::TooLarge);
    CHECK(out.empty());
}

TEST(preset_large_chunk_of_the_full_budget)
{
    // The full wire payload: 64 MiB of component state.
    const std::size_t n = 64u * 1024u * 1024u;
    Bytes comp(n);
    for (std::size_t i = 0; i < n; ++i)
        comp[i] = static_cast<std::uint8_t>((i * 2654435761u) >> 24);

    Bytes c;
    CHECK(buildVst3Preset(kClass, comp.data(), comp.size(), c).ok());
    CHECK_EQ(c.size(), n + 48u + 28u);
    CHECK_EQ(get64(c, 40), 48u + static_cast<std::uint64_t>(n));

    std::string cls;
    Bytes out;
    CHECK(readComp(c, cls, out).ok());
    CHECK_EQ(out.size(), n);
    CHECK(out == comp);
    CHECK_EQ(fnv1a64(out.data(), out.size()), fnv1a64(comp.data(), comp.size()));

    // A chunk size one byte too large for the container is refused even at this size.
    put64(c, c.size() - 8, static_cast<std::uint64_t>(n) + 1000000);
    CHECK(refuses(c, PresetError::BadChunkRange));
}

PHYLO_TEST_MAIN
