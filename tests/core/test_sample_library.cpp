#include "phylo/Wav.h"
#include "phylo/samples/SampleLibrary.h"

#include "check.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define PHYLO_GETPID _getpid
#else
#include <unistd.h>
#define PHYLO_GETPID getpid
#endif

using namespace phylo::samples;
namespace fs = std::filesystem;

namespace
{

// A fresh folder under the system temp directory, removed (only that folder) on scope exit.
// The library folder is a child that does not exist yet, so open() has to create it.
struct TempDir
{
    fs::path root;
    std::string lib; // UTF-8 path of root/"lib"

    TempDir()
    {
        static int counter = 0;
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        root = fs::temp_directory_path() /
               ("phylo_samplelib_" + std::to_string(PHYLO_GETPID()) + "_" + std::to_string(++counter) + "_" +
                std::to_string(static_cast<long long>(ticks)));
        std::error_code ec;
        fs::create_directories(root, ec);
        lib = (root / "lib").u8string();
    }

    ~TempDir()
    {
        std::error_code ec;
        const fs::path tmp = fs::temp_directory_path(ec);
        const std::string r = root.u8string();
        const std::string t = tmp.u8string();
        if (!r.empty() && r.size() > t.size() && r.compare(0, t.size(), t) == 0 &&
            root.filename().u8string().rfind("phylo_samplelib_", 0) == 0)
            fs::remove_all(root, ec);
    }

    fs::path dir() const { return fs::u8path(lib); }
    fs::path file(const std::string& name) const { return dir() / fs::u8path(name); }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

std::string readBytes(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeBytes(const fs::path& p, const std::string& bytes)
{
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::vector<float> ramp(std::size_t count, float seed = 0.0f)
{
    std::vector<float> v(count);
    for (std::size_t i = 0; i < count; ++i)
        v[i] = std::sin(0.01f * static_cast<float>(i) + seed) * 0.9f;
    return v;
}

bool sameBits(const std::vector<float>& a, const std::vector<float>& b)
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}

bool writeWav(const fs::path& p, const std::vector<float>& s, int ch, int rate = 48000)
{
    return phylo::writeWavFloat(p.u8string(), s, ch, rate);
}

bool readWav(const fs::path& p, std::vector<float>& s, int& ch, int& rate)
{
    return phylo::readWavFloat(p.u8string(), s, ch, rate);
}

std::size_t countEntries(const fs::path& dir)
{
    std::size_t n = 0;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        ++n;
    return n;
}

bool hasWarningWith(const SampleLibrary& lib, const std::string& text)
{
    for (const std::string& w : lib.warnings())
    {
        if (w.find(text) != std::string::npos)
            return true;
    }
    return false;
}

// True when `s` is well-formed UTF-8 (enough for these tests: no overlongs checks).
bool validUtf8(const std::string& s)
{
    std::size_t i = 0;
    while (i < s.size())
    {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t extra = 0;
        if (c < 0x80)
            extra = 0;
        else if ((c & 0xE0) == 0xC0)
            extra = 1;
        else if ((c & 0xF0) == 0xE0)
            extra = 2;
        else if ((c & 0xF8) == 0xF0)
            extra = 3;
        else
            return false;
        for (std::size_t k = 1; k <= extra; ++k)
        {
            if (i + k >= s.size() || (static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80)
                return false;
        }
        i += extra + 1;
    }
    return true;
}

bool sameEntry(const LibraryEntry& a, const LibraryEntry& b)
{
    return a.file == b.file && a.name == b.name && a.slot == b.slot && a.rootNote == b.rootNote &&
           a.channels == b.channels && a.frames == b.frames && a.missing == b.missing;
}

const std::string kHeaderLine = std::string(kIndexHeader) + "\n";

} // namespace

TEST(open_creates_folder_and_parents)
{
    TempDir t;
    const std::string deep = (t.root / "a" / "b" / "lib").u8string();
    SampleLibrary lib(deep);
    CHECK(lib.open().ok);
    CHECK(fs::is_directory(fs::u8path(deep)));
    CHECK(lib.entries().empty());
    CHECK(lib.warnings().empty());
    CHECK(lib.firstFreeSlot() == 0);
    CHECK(!fs::exists(fs::u8path(deep) / "index.txt"));
    CHECK_EQ(countEntries(fs::u8path(deep)), 0u);
}

TEST(add_writes_float_wav_and_index)
{
    TempDir t;
    SampleLibrary lib(t.lib);
    CHECK(lib.open().ok);
    const std::vector<float> s = ramp(2000);
    std::string file;
    const LibraryResult r = lib.add("kick", 2, s, file);
    CHECK(r.ok);
    CHECK(file == "kick.wav");
    CHECK(fs::exists(t.file(file)));

    std::vector<float> back;
    int ch = 0;
    int rate = 0;
    CHECK(readWav(t.file(file), back, ch, rate));
    CHECK_EQ(ch, 2);
    CHECK_EQ(rate, 48000);
    CHECK(sameBits(back, s));

    CHECK_EQ(lib.entries().size(), 1u);
    const LibraryEntry* e = lib.find(file);
    CHECK(e != nullptr);
    if (e)
    {
        CHECK(e->slot == kNoSlot);
        CHECK_EQ(e->rootNote, kDefaultRootNote);
        CHECK_EQ(e->channels, 2);
        CHECK_EQ(e->frames, 1000u);
        CHECK(e->name == "kick");
        CHECK(!e->missing);
    }
    const std::string idx = readBytes(t.file("index.txt"));
    CHECK(idx.compare(0, kHeaderLine.size(), kHeaderLine) == 0);
    CHECK(idx.find("kick.wav\t-\t60\tkick\n") != std::string::npos);
    CHECK(!fs::exists(t.file("index.txt.tmp")));
}

TEST(persistence_across_restart)
{
    TempDir t;
    {
        SampleLibrary lib(t.lib);
        CHECK(lib.open().ok);
        std::string a, b, c;
        CHECK(lib.add("kick", 1, ramp(100, 0.1f), a).ok);
        CHECK(lib.add("snare drum", 2, ramp(200, 0.2f), b).ok);
        CHECK(lib.add("hat", 1, ramp(50, 0.3f), c).ok);
        CHECK(lib.assign(a, 0).ok);
        CHECK(lib.assign(b, 127).ok);
        CHECK(lib.setRootNote(b, 36).ok);
        CHECK(lib.rename(c, "Closed Hat").ok);
    }
    SampleLibrary first(t.lib);
    CHECK(first.open().ok);
    CHECK(first.warnings().empty());
    CHECK_EQ(first.entries().size(), 3u);

    // Compare against a second independent open, and against known values.
    SampleLibrary second(t.lib);
    CHECK(second.open().ok);
    CHECK_EQ(second.entries().size(), 3u);
    for (std::size_t i = 0; i < first.entries().size() && i < second.entries().size(); ++i)
        CHECK(sameEntry(first.entries()[i], second.entries()[i]));

    const LibraryEntry* kick = first.find("kick.wav");
    const LibraryEntry* snare = first.find("snare_drum.wav");
    const LibraryEntry* hat = first.find("hat.wav");
    CHECK(kick && snare && hat);
    if (kick && snare && hat)
    {
        CHECK_EQ(kick->slot, 0);
        CHECK(kick->name == "kick");
        CHECK_EQ(kick->rootNote, kDefaultRootNote);
        CHECK_EQ(kick->channels, 1);
        CHECK_EQ(kick->frames, 100u);
        CHECK_EQ(snare->slot, 127);
        CHECK(snare->name == "snare drum");
        CHECK_EQ(snare->rootNote, 36);
        CHECK_EQ(snare->channels, 2);
        CHECK_EQ(snare->frames, 100u);
        CHECK_EQ(hat->slot, kNoSlot);
        CHECK(hat->name == "Closed Hat");
        CHECK_EQ(hat->frames, 50u);
    }
}

TEST(assign_rules)
{
    TempDir t;
    SampleLibrary lib(t.lib);
    CHECK(lib.open().ok);
    std::string a, b;
    CHECK(lib.add("a", 1, ramp(10), a).ok);
    CHECK(lib.add("b", 1, ramp(10), b).ok);

    CHECK(!lib.assign(a, -1).ok);
    CHECK(!lib.assign(a, 128).ok);
    CHECK(!lib.assign("nope.wav", 3).ok);
    CHECK(!lib.assign(a, 128).message.empty());

    CHECK(lib.assign(a, 5).ok);
    CHECK(lib.inSlot(5) == lib.find(a));
    CHECK(lib.assign(a, 5).ok); // same slot again

    // Taking an occupied slot unassigns the previous occupant, which stays in the library.
    CHECK(lib.assign(b, 5).ok);
    CHECK(lib.inSlot(5) == lib.find(b));
    CHECK(lib.find(a) != nullptr);
    if (lib.find(a))
        CHECK_EQ(lib.find(a)->slot, kNoSlot);
    CHECK_EQ(lib.entries().size(), 2u);

    // Moving frees the old slot.
    CHECK(lib.assign(b, 9).ok);
    CHECK(lib.inSlot(5) == nullptr);
    CHECK(lib.inSlot(9) == lib.find(b));

    // clearSlot keeps the sample.
    CHECK(lib.clearSlot(9).ok);
    CHECK(lib.inSlot(9) == nullptr);
    CHECK(lib.find(b) != nullptr);
    if (lib.find(b))
        CHECK_EQ(lib.find(b)->slot, kNoSlot);
    CHECK_EQ(lib.entries().size(), 2u);
    CHECK(lib.clearSlot(9).ok); // empty slot
    CHECK(!lib.clearSlot(128).ok);
    CHECK(!lib.clearSlot(-1).ok);
    CHECK(lib.inSlot(-1) == nullptr);
    CHECK(lib.inSlot(128) == nullptr);
    CHECK(lib.find("zzz") == nullptr);

    CHECK(!lib.setRootNote(a, 128).ok);
    CHECK(!lib.setRootNote(a, -1).ok);
    CHECK(!lib.setRootNote("nope.wav", 60).ok);
    CHECK(lib.setRootNote(a, 0).ok);
    CHECK(lib.setRootNote(a, 127).ok);
    CHECK(lib.find(a) != nullptr);
    if (lib.find(a))
        CHECK_EQ(lib.find(a)->rootNote, 127);
    CHECK(!lib.rename("nope.wav", "x").ok);
}

TEST(first_free_slot_and_full_library)
{
    TempDir t;
    SampleLibrary lib(t.lib);
    CHECK(lib.open().ok);
    CHECK_EQ(lib.firstFreeSlot(), 0);
    std::vector<std::string> files;
    for (int i = 0; i < kSlotCount; ++i)
    {
        std::string f;
        CHECK(lib.add("s", 1, ramp(1, static_cast<float>(i)), f).ok);
        files.push_back(f);
    }
    CHECK_EQ(lib.entries().size(), 128u);
    CHECK(lib.assign(files[0], 1).ok);
    CHECK_EQ(lib.firstFreeSlot(), 0);
    CHECK(lib.assign(files[1], 0).ok);
    CHECK_EQ(lib.firstFreeSlot(), 2);
    for (int i = 2; i < kSlotCount; ++i)
        CHECK(lib.assign(files[static_cast<std::size_t>(i)], i).ok);
    CHECK_EQ(lib.firstFreeSlot(), kNoSlot);
    CHECK(lib.clearSlot(77).ok);
    CHECK_EQ(lib.firstFreeSlot(), 77);
}

TEST(unique_names_never_overwrite)
{
    TempDir t;
    SampleLibrary lib(t.lib);
    CHECK(lib.open().ok);
    std::string f1, f2, f3;
    CHECK(lib.add("kick", 1, ramp(30, 1.0f), f1).ok);
    const std::string bytes1 = readBytes(t.file(f1));
    CHECK(lib.add("kick", 1, ramp(30, 2.0f), f2).ok);
    CHECK(lib.add("kick", 1, ramp(30, 3.0f), f3).ok);
    CHECK(f1 == "kick.wav");
    CHECK(f2 == "kick-2.wav");
    CHECK(f3 == "kick-3.wav");
    CHECK(readBytes(t.file(f1)) == bytes1);
    CHECK(fs::exists(t.file(f2)));
    CHECK(fs::exists(t.file(f3)));
}

TEST(case_insensitive_collision)
{
    TempDir t;
    SampleLibrary lib(t.lib);
    CHECK(lib.open().ok);
    std::error_code ec;
    fs::create_directories(t.dir(), ec);
    const std::string stray = "not a wav, a user's file";
    writeBytes(t.file("kick.wav"), stray);
    std::string f;
    CHECK(lib.add("Kick", 1, ramp(10), f).ok);
    CHECK(f != "Kick.wav");
    CHECK(f == "Kick-2.wav");
    CHECK(readBytes(t.file("kick.wav")) == stray);

    // A directory in the way counts too.
    fs::create_directories(t.file("Snare-2.wav"), ec);
    std::string g1, g2;
    CHECK(lib.add("snare", 1, ramp(10), g1).ok);
    CHECK(lib.add("SNARE", 1, ramp(10), g2).ok);
    CHECK(g1 == "snare.wav");
    CHECK(g2 == "SNARE-3.wav");
}

TEST(hostile_names_stay_inside_folder)
{
    TempDir t;
    SampleLibrary lib(t.lib);
    CHECK(lib.open().ok);
    std::string snowmen;
    for (int i = 0; i < 100; ++i)
        snowmen += "\xE2\x98\x83"; // 300 bytes, three per character
    const std::vector<std::string> names = {
        "../../etc/passwd", "a/b\\c", "", "...", "index.txt", std::string(300, 'x'), "tab\there\nnewline",
        "s\xC3\xA4mple \xE2\x98\x83", snowmen, "CON", "  ", ".."};
    std::size_t n = 0;
    for (const std::string& name : names)
    {
        std::string f;
        const LibraryResult r = lib.add(name, 1, ramp(5, static_cast<float>(n)), f);
        CHECK(r.ok);
        ++n;
        CHECK(!f.empty());
        CHECK(f.find('/') == std::string::npos);
        CHECK(f.find('\\') == std::string::npos);
        CHECK(f != "index.txt");
        CHECK(f.size() > 4 && f.compare(f.size() - 4, 4, ".wav") == 0);
        CHECK(f.size() <= 48 + 4 + 4);
        const fs::path p = t.file(f);
        CHECK(p.parent_path() == t.dir());
        CHECK(fs::exists(p));
        const LibraryEntry* e = lib.find(f);
        CHECK(e != nullptr);
        if (e)
        {
            CHECK(e->name.size() <= kMaxNameBytes);
            CHECK(!e->name.empty());
            CHECK(validUtf8(e->name));
            CHECK(e->name.find('\t') == std::string::npos);
            CHECK(e->name.find('\n') == std::string::npos);
        }
    }
    CHECK(lib.find("sample.wav") != nullptr);
    // Nothing outside the lib folder was created.
    CHECK_EQ(countEntries(t.root), 1u);
    CHECK(!fs::exists(t.root / "etc"));

    // And the index round-trips.
    SampleLibrary again(t.lib);
    CHECK(again.open().ok);
    CHECK(again.warnings().empty());
    CHECK_EQ(again.entries().size(), lib.entries().size());
    for (std::size_t i = 0; i < again.entries().size() && i < lib.entries().size(); ++i)
        CHECK(sameEntry(again.entries()[i], lib.entries()[i]));
}

TEST(add_refusals_write_nothing)
{
    TempDir t;
    SampleLibrary lib(t.lib);
    CHECK(lib.open().ok);
    std::string f;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    CHECK(!lib.add("x", 0, ramp(10), f).ok);
    CHECK(!lib.add("x", 3, ramp(9), f).ok);
    CHECK(!lib.add("x", -1, ramp(10), f).ok);
    CHECK(!lib.add("x", 1, std::vector<float>(), f).ok);
    CHECK(!lib.add("x", 2, ramp(9), f).ok); // odd count for stereo
    std::vector<float> bad = ramp(10);
    bad[4] = nan;
    CHECK(!lib.add("x", 1, bad, f).ok);
    bad[4] = inf;
    CHECK(!lib.add("x", 1, bad, f).ok);
    bad[4] = -inf;
    CHECK(!lib.add("x", 1, bad, f).ok);
    {
        const std::vector<float> tooLong(static_cast<std::size_t>(kMaxSampleFrames) + 1, 0.25f);
        const LibraryResult r = lib.add("x", 1, tooLong, f);
        CHECK(!r.ok);
        CHECK(!r.message.empty());
    }
    CHECK(f.empty());
    CHECK(lib.entries().empty());
    CHECK_EQ(countEntries(t.dir()), 0u);
}

TEST(max_length_accepted)
{
    TempDir t;
    SampleLibrary lib(t.lib);
    CHECK(lib.open().ok);
    const std::vector<float> s(static_cast<std::size_t>(kMaxSampleFrames), 0.5f);
    std::string f;
    CHECK(lib.add("long", 1, s, f).ok);
    CHECK(lib.find(f) != nullptr);
    if (lib.find(f))
        CHECK_EQ(lib.find(f)->frames, kMaxSampleFrames);

    SampleLibrary again(t.lib);
    CHECK(again.open().ok);
    CHECK(again.warnings().empty());
    CHECK(again.find(f) != nullptr && !again.find(f)->missing);
}

TEST(missing_file_keeps_slot)
{
    TempDir t;
    const std::vector<float> s = ramp(64);
    std::string f;
    {
        SampleLibrary lib(t.lib);
        CHECK(lib.open().ok);
        CHECK(lib.add("tom", 1, s, f).ok);
        CHECK(lib.assign(f, 12).ok);
    }
    const std::string bytes = readBytes(t.file(f));
    std::error_code ec;
    fs::remove(t.file(f), ec); // this test's own temp folder
    CHECK(!fs::exists(t.file(f)));

    SampleLibrary lib(t.lib);
    CHECK(lib.open().ok);
    CHECK_EQ(lib.entries().size(), 1u);
    const LibraryEntry* e = lib.find(f);
    CHECK(e != nullptr);
    if (e)
    {
        CHECK(e->missing);
        CHECK_EQ(e->slot, 12);
        CHECK_EQ(e->frames, 0u);
    }
    SampleSlotSet set;
    CHECK(lib.buildSlotSet(set).ok);
    CHECK(set.empty());
    CHECK(!lib.assign(f, 13).ok);
    CHECK(lib.clearSlot(12).ok);
    CHECK(lib.assign(f, 13).ok == false);
    CHECK(lib.setRootNote(f, 40).ok); // metadata edits still work

    // Put it back.
    writeBytes(t.file(f), bytes);
    SampleLibrary back(t.lib);
    CHECK(back.open().ok);
    CHECK(back.find(f) != nullptr && !back.find(f)->missing);
    if (back.find(f))
    {
        CHECK_EQ(back.find(f)->frames, 64u);
        CHECK_EQ(back.find(f)->rootNote, 40);
        CHECK_EQ(back.find(f)->slot, kNoSlot); // we cleared it above
    }
}

TEST(missing_file_slot_survives_until_back)
{
    TempDir t;
    std::string f;
    {
        SampleLibrary lib(t.lib);
        CHECK(lib.open().ok);
        CHECK(lib.add("tom", 1, ramp(64), f).ok);
        CHECK(lib.assign(f, 12).ok);
    }
    const std::string bytes = readBytes(t.file(f));
    std::error_code ec;
    fs::remove(t.file(f), ec);
    {
        SampleLibrary lib(t.lib);
        CHECK(lib.open().ok);
        CHECK(lib.find(f) != nullptr && lib.find(f)->missing);
        if (lib.find(f))
            CHECK_EQ(lib.find(f)->slot, 12);
    }
    writeBytes(t.file(f), bytes);
    SampleLibrary back(t.lib);
    CHECK(back.open().ok);
    CHECK(back.find(f) != nullptr && !back.find(f)->missing);
    if (back.find(f))
        CHECK_EQ(back.find(f)->slot, 12);
    SampleSlotSet set;
    CHECK(back.buildSlotSet(set).ok);
    CHECK_EQ(set.size(), 1u);
}

TEST(adoption_of_hand_placed_files)
{
    TempDir t;
    std::error_code ec;
    fs::create_directories(t.dir(), ec);
    const std::vector<float> good = ramp(48, 0.5f);
    CHECK(writeWav(t.file("zebra.wav"), good, 1));
    CHECK(writeWav(t.file("Apple.WAV"), ramp(20), 2));
    CHECK(writeWav(t.file("mango.wav"), ramp(30), 1));
    CHECK(writeWav(t.file("wrongrate.wav"), ramp(30), 1, 44100));
    writeBytes(t.file("x.wav"), "this is not a wav file at all");
    writeBytes(t.file("notes.txt"), "hello");
    fs::create_directories(t.file("sub.wav"), ec); // a directory named like a wav
    fs::create_directories(t.file("subdir"), ec);
    const std::string wrongBytes = readBytes(t.file("wrongrate.wav"));
    const std::string xBytes = readBytes(t.file("x.wav"));

    SampleLibrary lib(t.lib);
    CHECK(lib.open().ok);
    CHECK_EQ(lib.entries().size(), 3u);
    if (lib.entries().size() == 3)
    {
        // Sorted by file name (byte order): "Apple.WAV" < "mango.wav" < "zebra.wav".
        CHECK(lib.entries()[0].file == "Apple.WAV");
        CHECK(lib.entries()[1].file == "mango.wav");
        CHECK(lib.entries()[2].file == "zebra.wav");
        CHECK(lib.entries()[0].name == "Apple");
        CHECK(lib.entries()[2].name == "zebra");
        CHECK_EQ(lib.entries()[0].channels, 2);
        CHECK_EQ(lib.entries()[0].frames, 10u);
        CHECK_EQ(lib.entries()[2].frames, 48u);
        for (const LibraryEntry& e : lib.entries())
        {
            CHECK_EQ(e.slot, kNoSlot);
            CHECK_EQ(e.rootNote, kDefaultRootNote);
            CHECK(!e.missing);
        }
    }
    CHECK(lib.find("wrongrate.wav") == nullptr);
    CHECK(lib.find("x.wav") == nullptr);
    CHECK_EQ(lib.warnings().size(), 2u);
    CHECK(hasWarningWith(lib, "wrongrate.wav"));
    CHECK(hasWarningWith(lib, "x.wav"));
    CHECK(readBytes(t.file("wrongrate.wav")) == wrongBytes);
    CHECK(readBytes(t.file("x.wav")) == xBytes);
    CHECK(fs::exists(t.file("notes.txt")));
    CHECK(fs::is_directory(t.file("sub.wav")));
    CHECK(fs::exists(t.file("index.txt")));

    // Adopted samples persist, and the bad files are warned about again, never absorbed.
    SampleLibrary again(t.lib);
    CHECK(again.open().ok);
    CHECK_EQ(again.entries().size(), 3u);
    CHECK_EQ(again.warnings().size(), 2u);
}

TEST(damaged_index_garbage_first_line)
{
    TempDir t;
    std::error_code ec;
    fs::create_directories(t.dir(), ec);
    CHECK(writeWav(t.file("one.wav"), ramp(10), 1));
    const std::string garbage = "this is not an index\nfoo\tbar\n";
    writeBytes(t.file("index.txt"), garbage);

    SampleLibrary lib(t.lib);
    CHECK(lib.open().ok);
    CHECK(!lib.warnings().empty());
    CHECK_EQ(lib.entries().size(), 1u); // adopted from the folder
    CHECK(readBytes(t.file("index.txt.bad")) == garbage);
    const std::string idx = readBytes(t.file("index.txt"));
    CHECK(idx.compare(0, kHeaderLine.size(), kHeaderLine) == 0);
    CHECK(idx.find("one.wav\t-\t60\tone\n") != std::string::npos);

    // The next open finds a clean index and leaves the .bad copy as it was.
    SampleLibrary again(t.lib);
    CHECK(again.open().ok);
    CHECK(again.warnings().empty());
    CHECK(readBytes(t.file("index.txt.bad")) == garbage);
}

TEST(damaged_index_bad_lines_among_good)
{
    TempDir t;
    std::error_code ec;
    fs::create_directories(t.dir(), ec);
    CHECK(writeWav(t.file("a.wav"), ramp(10), 1));
    CHECK(writeWav(t.file("b.wav"), ramp(11), 1));
    CHECK(writeWav(t.file("c.wav"), ramp(12), 2));
    CHECK(writeWav(t.file("d.wav"), ramp(14), 2));
    CHECK(writeWav(t.file("e.wav"), ramp(16), 2));
    const std::string text = kHeaderLine +
                             "a.wav\t3\t50\tAlpha\n" +        // good
                             "b.wav\t999\t60\tBeta\n" +       // bad slot (line 3)
                             "garbage line\n" +               // wrong field count (line 4)
                             "c.wav\t-\t200\tGamma\n" +       // bad root (line 5)
                             "../escape.wav\t-\t60\tEsc\n" +  // separator (line 6)
                             "..\t-\t60\tDots\n" +            // dotted (line 7)
                             "\t-\t60\tNoFile\n" +            // empty file (line 8)
                             "index.txt\t-\t60\tSelf\n" +     // reserved (line 9)
                             "d.wav\t7\t61\tDelta\n";         // good
    writeBytes(t.file("index.txt"), text);

    SampleLibrary lib(t.lib);
    CHECK(lib.open().ok);
    CHECK(lib.find("a.wav") != nullptr);
    CHECK(lib.find("d.wav") != nullptr);
    if (lib.find("a.wav"))
    {
        CHECK_EQ(lib.find("a.wav")->slot, 3);
        CHECK_EQ(lib.find("a.wav")->rootNote, 50);
        CHECK(lib.find("a.wav")->name == "Alpha");
    }
    if (lib.find("d.wav"))
    {
        CHECK_EQ(lib.find("d.wav")->slot, 7);
        CHECK_EQ(lib.find("d.wav")->rootNote, 61);
    }
    // Skipped lines are not in the index; their files in the folder are adopted unassigned.
    CHECK(lib.find("b.wav") != nullptr && lib.find("b.wav")->slot == kNoSlot);
    CHECK(lib.find("c.wav") != nullptr && lib.find("c.wav")->slot == kNoSlot);
    CHECK(lib.find("../escape.wav") == nullptr);
    CHECK(lib.find("..") == nullptr);
    CHECK(lib.find("index.txt") == nullptr);
    for (const char* n : {"line 3", "line 4", "line 5", "line 6", "line 7", "line 8", "line 9"})
        CHECK(hasWarningWith(lib, n));
    CHECK(!hasWarningWith(lib, "line 2"));
    CHECK(!hasWarningWith(lib, "line 10"));
    CHECK(readBytes(t.file("index.txt.bad")) == text);
    SampleLibrary again(t.lib);
    CHECK(again.open().ok);
    CHECK(again.warnings().empty());
    CHECK_EQ(again.entries().size(), lib.entries().size());
}

TEST(damaged_index_duplicates)
{
    TempDir t;
    std::error_code ec;
    fs::create_directories(t.dir(), ec);
    CHECK(writeWav(t.file("a.wav"), ramp(10), 1));
    CHECK(writeWav(t.file("b.wav"), ramp(11), 1));
    CHECK(writeWav(t.file("c.wav"), ramp(12), 1));
    const std::string text = kHeaderLine + "a.wav\t5\t60\tA\n" + "b.wav\t5\t60\tB\n" + "a.wav\t6\t60\tA again\n" +
                             "c.wav\t6\t60\tC\n";
    writeBytes(t.file("index.txt"), text);

    SampleLibrary lib(t.lib);
    CHECK(lib.open().ok);
    CHECK_EQ(lib.entries().size(), 3u);
    CHECK(lib.find("a.wav") != nullptr && lib.find("b.wav") != nullptr && lib.find("c.wav") != nullptr);
    if (lib.find("a.wav"))
    {
        CHECK_EQ(lib.find("a.wav")->slot, 5);
        CHECK(lib.find("a.wav")->name == "A");
    }
    if (lib.find("b.wav"))
        CHECK_EQ(lib.find("b.wav")->slot, kNoSlot);
    if (lib.find("c.wav"))
        CHECK_EQ(lib.find("c.wav")->slot, 6); // the dropped duplicate line did not take slot 6
    CHECK(hasWarningWith(lib, "line 3"));
    CHECK(hasWarningWith(lib, "line 4"));
    CHECK(readBytes(t.file("index.txt.bad")) == text);
    const std::string idx = readBytes(t.file("index.txt"));
    CHECK(idx.find("b.wav\t-\t60\tB\n") != std::string::npos);
}

TEST(crlf_index_accepted)
{
    TempDir t;
    std::error_code ec;
    fs::create_directories(t.dir(), ec);
    CHECK(writeWav(t.file("a.wav"), ramp(10), 1));
    CHECK(writeWav(t.file("b.wav"), ramp(10), 2));
    const std::string text = std::string(kIndexHeader) + "\r\na.wav\t4\t48\tAlpha\r\nb.wav\t-\t60\tBeta\r\n";
    writeBytes(t.file("index.txt"), text);

    SampleLibrary lib(t.lib);
    CHECK(lib.open().ok);
    CHECK(lib.warnings().empty());
    CHECK_EQ(lib.entries().size(), 2u);
    CHECK(lib.find("a.wav") != nullptr && lib.find("b.wav") != nullptr);
    if (lib.find("a.wav"))
    {
        CHECK_EQ(lib.find("a.wav")->slot, 4);
        CHECK_EQ(lib.find("a.wav")->rootNote, 48);
        CHECK(lib.find("a.wav")->name == "Alpha");
    }
    if (lib.find("b.wav"))
        CHECK(lib.find("b.wav")->name == "Beta");
    CHECK(!fs::exists(t.file("index.txt.bad")));
    CHECK(readBytes(t.file("index.txt")) == text); // untouched
}

TEST(crash_safety_stale_tmp_and_unchanged_open)
{
    TempDir t;
    std::string f;
    {
        SampleLibrary lib(t.lib);
        CHECK(lib.open().ok);
        CHECK(lib.add("a", 1, ramp(10), f).ok);
        CHECK(lib.assign(f, 2).ok);
    }
    writeBytes(t.file("index.txt.tmp"), "half written garbage");
    const std::string before = readBytes(t.file("index.txt"));
    std::error_code ec;
    const auto timeBefore = fs::last_write_time(t.file("index.txt"), ec);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    SampleLibrary lib(t.lib);
    CHECK(lib.open().ok);
    CHECK(lib.warnings().empty());
    CHECK_EQ(lib.entries().size(), 1u);
    CHECK(lib.find(f) != nullptr);
    if (lib.find(f))
        CHECK_EQ(lib.find(f)->slot, 2);
    CHECK(readBytes(t.file("index.txt")) == before);
    CHECK(fs::last_write_time(t.file("index.txt"), ec) == timeBefore);
    CHECK(!fs::exists(t.file("index.txt.bad")));

    SampleLibrary again(t.lib);
    CHECK(again.open().ok);
    CHECK(readBytes(t.file("index.txt")) == before);
    CHECK(fs::last_write_time(t.file("index.txt"), ec) == timeBefore);

    // The next save replaces the stale temporary file.
    CHECK(lib.setRootNote(f, 70).ok);
    CHECK(!fs::exists(t.file("index.txt.tmp")));
    CHECK(readBytes(t.file("index.txt")).find("\t70\t") != std::string::npos);
    SampleLibrary last(t.lib);
    CHECK(last.open().ok);
    CHECK(last.find(f) != nullptr);
    if (last.find(f))
        CHECK_EQ(last.find(f)->rootNote, 70);
}

TEST(build_slot_set)
{
    TempDir t;
    SampleLibrary lib(t.lib);
    CHECK(lib.open().ok);
    const std::vector<float> a = ramp(100, 0.1f);
    const std::vector<float> b = ramp(60, 0.2f);
    const std::vector<float> c = ramp(7, 0.3f);
    std::string fa, fb, fc, fd;
    CHECK(lib.add("hi", 1, a, fa).ok);
    CHECK(lib.add("lo", 2, b, fb).ok);
    CHECK(lib.add("unassigned", 1, c, fc).ok);
    CHECK(lib.add("mid", 1, c, fd).ok);
    CHECK(lib.assign(fa, 90).ok);
    CHECK(lib.assign(fb, 3).ok);
    CHECK(lib.assign(fd, 40).ok);
    CHECK(lib.setRootNote(fb, 33).ok);

    SampleSlotSet set;
    CHECK(lib.buildSlotSet(set).ok);
    CHECK_EQ(set.size(), 3u);
    if (set.size() == 3)
    {
        CHECK_EQ(set[0].slot, 3);
        CHECK(set[0].name == "lo");
        CHECK_EQ(set[0].rootNote, 33);
        CHECK_EQ(set[0].channels, 2);
        CHECK(sameBits(set[0].samples, b));
        CHECK_EQ(set[1].slot, 40);
        CHECK(sameBits(set[1].samples, c));
        CHECK_EQ(set[1].rootNote, kDefaultRootNote);
        CHECK_EQ(set[2].slot, 90);
        CHECK(set[2].name == "hi");
        CHECK(sameBits(set[2].samples, a));
        CHECK_EQ(set[2].channels, 1);
    }

    // Empty library gives an empty set.
    TempDir t2;
    SampleLibrary empty(t2.lib);
    CHECK(empty.open().ok);
    SampleSlotSet none;
    none.resize(2);
    CHECK(empty.buildSlotSet(none).ok);
    CHECK(none.empty());

    // A file replaced on disk after open() is caught.
    CHECK(writeWav(t.file(fb), ramp(60), 2, 44100)); // test overwrites its own file
    CHECK(!lib.buildSlotSet(set).ok);
    CHECK(set.empty());
    const LibraryResult r = lib.buildSlotSet(set);
    CHECK(r.message.find(fb) != std::string::npos);
    CHECK(set.empty());

    // A file replaced by one with NaN is caught too.
    std::vector<float> nanData = b;
    nanData[1] = std::numeric_limits<float>::quiet_NaN();
    CHECK(writeWav(t.file(fb), nanData, 2));
    CHECK(!lib.buildSlotSet(set).ok);
    CHECK(set.empty());
}

TEST(library_never_deletes_files)
{
    TempDir t;
    std::vector<std::pair<std::string, std::string>> made; // file, bytes
    SampleLibrary lib(t.lib);
    CHECK(lib.open().ok);
    for (int i = 0; i < 6; ++i)
    {
        std::string f;
        CHECK(lib.add(i % 2 ? "loop" : "hit", 1 + i % 2, ramp(40, static_cast<float>(i)), f).ok);
        made.emplace_back(f, readBytes(t.file(f)));
    }
    CHECK(lib.assign(made[0].first, 0).ok);
    CHECK(lib.assign(made[1].first, 0).ok);
    CHECK(lib.assign(made[2].first, 1).ok);
    CHECK(lib.clearSlot(1).ok);
    CHECK(lib.rename(made[3].first, "renamed").ok);
    CHECK(lib.setRootNote(made[4].first, 20).ok);
    CHECK(lib.open().ok);
    {
        SampleLibrary other(t.lib);
        CHECK(other.open().ok);
        std::string f;
        CHECK(other.add("hit", 1, ramp(5), f).ok);
        made.emplace_back(f, readBytes(t.file(f)));
    }
    CHECK(lib.open().ok);
    for (const auto& m : made)
    {
        CHECK(fs::exists(t.file(m.first)));
        CHECK(readBytes(t.file(m.first)) == m.second);
    }
    CHECK_EQ(lib.entries().size(), made.size());
}

TEST(reopen_picks_up_changes)
{
    TempDir t;
    SampleLibrary lib(t.lib);
    CHECK(lib.open().ok);
    std::string f;
    CHECK(lib.add("a", 1, ramp(10), f).ok);
    CHECK(writeWav(t.file("late.wav"), ramp(10), 1));
    CHECK(lib.open().ok);
    CHECK_EQ(lib.entries().size(), 2u);
    CHECK(lib.find("late.wav") != nullptr);
}

// ---------------------------------------------------------------------------------------
// Review fixes.

namespace
{

std::string indexOf(const std::string& tail)
{
    return kHeaderLine + tail;
}

// A file outside the library folder, with known contents, that nothing may write to.
struct Victim
{
    fs::path path;
    std::string bytes = "do not touch this file\n";

    explicit Victim(const TempDir& t) : path(t.root / "victim.txt")
    {
        std::error_code ec;
        fs::create_directories(t.root, ec);
        writeBytes(path, bytes);
    }
    bool untouched() const { return fs::exists(path) && readBytes(path) == bytes; }
};

} // namespace

#if !defined(_WIN32)

TEST(symlinked_tmp_is_never_written_through)
{
    TempDir t;
    const Victim v(t);
    SampleLibrary lib(t.lib);
    CHECK(lib.open().ok);
    std::string f;
    CHECK(lib.add("kick", 1, ramp(20), f).ok);
    const std::string before = readBytes(t.file("index.txt"));

    std::error_code ec;
    fs::create_symlink(v.path, t.file("index.txt.tmp"), ec);
    CHECK(!ec);

    const LibraryResult r = lib.setRootNote(f, 70);
    CHECK(!r.ok);
    CHECK(r.message.find("index.txt.tmp") != std::string::npos);
    CHECK(v.untouched());
    CHECK(fs::is_symlink(t.file("index.txt.tmp"))); // the user's symlink is still there
    CHECK(lib.find(f) != nullptr);
    if (lib.find(f))
        CHECK_EQ(lib.find(f)->rootNote, kDefaultRootNote);
    CHECK(readBytes(t.file("index.txt")) == before);

    // The same through every other way of saving.
    std::string g;
    CHECK(!lib.add("snare", 1, ramp(20), g).ok);
    CHECK(!lib.assign(f, 3).ok);
    CHECK(!lib.rename(f, "other").ok);
    CHECK(v.untouched());
    CHECK(fs::is_symlink(t.file("index.txt.tmp")));
}

TEST(symlinked_tmp_dangling_is_left_alone_too)
{
    TempDir t;
    const fs::path nowhere = t.root / "not_there.txt";
    SampleLibrary lib(t.lib);
    CHECK(lib.open().ok);
    std::string f;
    CHECK(lib.add("kick", 1, ramp(20), f).ok);
    std::error_code ec;
    fs::create_symlink(nowhere, t.file("index.txt.tmp"), ec);
    CHECK(!lib.setRootNote(f, 70).ok);
    CHECK(!fs::exists(nowhere)); // nothing was created through the link
    CHECK(fs::is_symlink(t.file("index.txt.tmp")));
}

TEST(symlinked_bad_is_never_written_through)
{
    TempDir t;
    const Victim v(t);
    std::error_code ec;
    fs::create_directories(t.dir(), ec);
    CHECK(writeWav(t.file("one.wav"), ramp(10), 1));
    const std::string garbage = "not an index\n";
    writeBytes(t.file("index.txt"), garbage);
    fs::create_symlink(v.path, t.file("index.txt.bad"), ec);
    CHECK(!ec);

    SampleLibrary lib(t.lib);
    CHECK(lib.open().ok);
    CHECK(v.untouched());
    CHECK(fs::is_symlink(t.file("index.txt.bad")));
    // The damaged index was still kept, under the next free name.
    CHECK(readBytes(t.file("index.txt.bad-2")) == garbage);
    CHECK(lib.find("one.wav") != nullptr);
    CHECK(readBytes(t.file("index.txt")).compare(0, kHeaderLine.size(), kHeaderLine) == 0);
}

TEST(symlinked_index_is_not_replaced)
{
    TempDir t;
    Victim v(t);
    v.bytes = indexOf("a.wav\t4\t48\tAlpha\n");
    writeBytes(v.path, v.bytes);
    std::error_code ec;
    fs::create_directories(t.dir(), ec);
    CHECK(writeWav(t.file("a.wav"), ramp(10), 1));
    fs::create_symlink(v.path, t.file("index.txt"), ec);
    CHECK(!ec);

    SampleLibrary lib(t.lib);
    CHECK(lib.open().ok);
    CHECK(hasWarningWith(lib, "index.txt"));
    CHECK(hasWarningWith(lib, "symlink"));
    CHECK(v.untouched());
    CHECK(fs::is_symlink(t.file("index.txt")));
    CHECK(!fs::exists(t.file("index.txt.tmp")));

    CHECK(!lib.setRootNote("a.wav", 10).ok);
    CHECK(!lib.assign("a.wav", 9).ok);
    std::string f;
    const LibraryResult r = lib.add("new", 1, ramp(10), f);
    CHECK(!r.ok);
    CHECK(r.message.find("index.txt") != std::string::npos);
    CHECK(!fs::exists(t.file("new.wav"))); // refused before a file was written
    CHECK(v.untouched());
    CHECK(fs::is_symlink(t.file("index.txt")));
    CHECK(!fs::exists(t.file("index.txt.tmp")));
}

TEST(hand_placed_names_the_index_cannot_hold_are_not_adopted)
{
    TempDir t;
    std::error_code ec;
    fs::create_directories(t.dir(), ec);
    const std::string names[] = {"my\tkick.wav", "line\nbreak.wav", "back\\slash.wav", "del\x7f.wav", "ctl\x01.wav",
                                 "col:on.wav"};
    std::vector<std::string> bytes;
    for (const std::string& n : names)
    {
        CHECK(writeWav(t.file(n), ramp(12), 1));
        bytes.push_back(readBytes(t.file(n)));
    }
    CHECK(writeWav(t.file("good.wav"), ramp(12), 1));
    writeBytes(t.file("index.txt"), indexOf("good.wav\t7\t60\tgood\n"));
    const std::string idx = readBytes(t.file("index.txt"));

    SampleLibrary lib(t.lib);
    CHECK(lib.open().ok);
    CHECK_EQ(lib.entries().size(), 1u);
    CHECK(lib.find("good.wav") != nullptr);
    CHECK_EQ(lib.warnings().size(), 6u);
    CHECK(hasWarningWith(lib, "rename the file to import it"));
    CHECK(hasWarningWith(lib, "the index cannot hold"));
    for (std::size_t i = 0; i < 6; ++i)
    {
        CHECK(fs::exists(t.file(names[i])));
        CHECK(readBytes(t.file(names[i])) == bytes[i]);
    }
    // Nothing was adopted, so nothing was saved: no new index, no .bad, no injected line.
    CHECK(readBytes(t.file("index.txt")) == idx);
    CHECK(!fs::exists(t.file("index.txt.bad")));

    std::error_code te;
    const auto stamp = fs::last_write_time(t.file("index.txt"), te);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    SampleLibrary again(t.lib);
    CHECK(again.open().ok);
    CHECK_EQ(again.entries().size(), 1u);
    CHECK(readBytes(t.file("index.txt")) == idx);
    CHECK(fs::last_write_time(t.file("index.txt"), te) == stamp);
    CHECK(!fs::exists(t.file("index.txt.bad")));
    CHECK_EQ(again.warnings().size(), 6u);
}

TEST(newline_name_cannot_inject_an_index_line)
{
    TempDir t;
    std::error_code ec;
    fs::create_directories(t.dir(), ec);
    CHECK(writeWav(t.file("x.wav\nevil.wav\t5\t60\thacked\n.wav"), ramp(12), 1));
    SampleLibrary lib(t.lib);
    CHECK(lib.open().ok);
    CHECK(lib.entries().empty());
    CHECK(!fs::exists(t.file("index.txt")));
}

#endif // !_WIN32

TEST(index_names_with_backslash_control_or_colon_are_skipped)
{
    TempDir t;
    std::error_code ec;
    fs::create_directories(t.dir(), ec);
    CHECK(writeWav(t.file("ok.wav"), ramp(10), 1));
    const std::string text = indexOf(std::string("ok.wav\t1\t60\tOk\n") +        // line 2 good
                                     "back\\slash.wav\t-\t60\tB\n" +             // line 3
                                     "ctl\x01x.wav\t-\t60\tC\n" +                // line 4
                                     "del\x7f.wav\t-\t60\tD\n" +                 // line 5
                                     "C:foo.wav\t-\t60\tE\n" +                   // line 6
                                     "foo.wav:stream\t-\t60\tF\n");              // line 7
    writeBytes(t.file("index.txt"), text);
    SampleLibrary lib(t.lib);
    CHECK(lib.open().ok);
    CHECK_EQ(lib.entries().size(), 1u);
    CHECK(lib.find("ok.wav") != nullptr);
    for (const char* n : {"line 3", "line 4", "line 5", "line 6", "line 7"})
        CHECK(hasWarningWith(lib, n));
    CHECK(lib.find("C:foo.wav") == nullptr);
    CHECK(readBytes(t.file("index.txt.bad")) == text);
}

TEST(second_damaged_index_does_not_overwrite_the_first_backup)
{
    TempDir t;
    std::error_code ec;
    fs::create_directories(t.dir(), ec);
    CHECK(writeWav(t.file("one.wav"), ramp(10), 1));
    const std::string g1 = indexOf("one.wav\t4\t60\tOne\nthis line is damaged\n");
    writeBytes(t.file("index.txt"), g1);
    {
        SampleLibrary lib(t.lib);
        CHECK(lib.open().ok);
        CHECK(lib.find("one.wav") != nullptr);
        if (lib.find("one.wav"))
            CHECK_EQ(lib.find("one.wav")->slot, 4);
    }
    CHECK(readBytes(t.file("index.txt.bad")) == g1);

    const std::string g2 = "garbage number two\n";
    writeBytes(t.file("index.txt"), g2);
    {
        SampleLibrary lib(t.lib);
        CHECK(lib.open().ok);
    }
    CHECK(readBytes(t.file("index.txt.bad")) == g1); // the first backup is as it was
    CHECK(readBytes(t.file("index.txt.bad-2")) == g2);

    const std::string g3 = "garbage number three\n";
    writeBytes(t.file("index.txt"), g3);
    SampleLibrary lib(t.lib);
    CHECK(lib.open().ok);
    CHECK(readBytes(t.file("index.txt.bad")) == g1);
    CHECK(readBytes(t.file("index.txt.bad-2")) == g2);
    CHECK(readBytes(t.file("index.txt.bad-3")) == g3);

    // The backups are not samples and not index entries, and never take a sample's name.
    SampleLibrary again(t.lib);
    CHECK(again.open().ok);
    CHECK(again.warnings().empty());
    CHECK_EQ(again.entries().size(), 1u);
    writeBytes(t.file("index.txt"), indexOf("index.txt.bad-2\t-\t60\tx\none.wav\t-\t60\tOne\n"));
    SampleLibrary reserved(t.lib);
    CHECK(reserved.open().ok);
    CHECK(reserved.find("index.txt.bad-2") == nullptr);
    CHECK(hasWarningWith(reserved, "line 2"));
    std::string f;
    CHECK(reserved.add("index.txt.bad", 1, ramp(4), f).ok);
    CHECK(f != "index.txt.bad" && f != "index.txt.bad-2" && f != "index.txt.bad-3");
    CHECK(readBytes(t.file("index.txt.bad-2")) == g2);
}

TEST(index_with_a_utf8_bom_is_read)
{
    TempDir t;
    std::error_code ec;
    fs::create_directories(t.dir(), ec);
    CHECK(writeWav(t.file("a.wav"), ramp(10), 1));
    const std::string text = "\xEF\xBB\xBF" + indexOf("a.wav\t4\t48\tAlpha\n");
    writeBytes(t.file("index.txt"), text);
    SampleLibrary lib(t.lib);
    CHECK(lib.open().ok);
    CHECK(lib.warnings().empty());
    CHECK_EQ(lib.entries().size(), 1u);
    if (lib.find("a.wav"))
    {
        CHECK_EQ(lib.find("a.wav")->slot, 4);
        CHECK(lib.find("a.wav")->name == "Alpha");
    }
    CHECK(!fs::exists(t.file("index.txt.bad")));
    CHECK(readBytes(t.file("index.txt")) == text); // not rewritten
}

TEST(index_from_a_newer_version_is_left_alone)
{
    for (const char* header : {"phylotone.samples.index.v2", "phylotone.samples.index.v10", "phylotone.samples.index.v0",
                               "phylotone.samples.index."})
    {
        TempDir t;
        std::error_code ec;
        fs::create_directories(t.dir(), ec);
        CHECK(writeWav(t.file("a.wav"), ramp(10), 1));
        const std::string wavBytes = readBytes(t.file("a.wav"));
        const std::string text = std::string(header) + "\na.wav\t4\t48\tAlpha\tnew field\n";
        writeBytes(t.file("index.txt"), text);

        SampleLibrary lib(t.lib);
        const LibraryResult r = lib.open();
        CHECK(!r.ok);
        CHECK(r.message.find("newer version of Phylotone") != std::string::npos);
        CHECK(r.message.find("left as it is") != std::string::npos);
        CHECK(lib.entries().empty());
        CHECK(readBytes(t.file("index.txt")) == text);
        CHECK(!fs::exists(t.file("index.txt.bad")));
        CHECK(!fs::exists(t.file("index.txt.tmp")));

        // The library cannot be used to replace it either.
        std::string f;
        CHECK(!lib.add("new", 1, ramp(10), f).ok);
        CHECK(!fs::exists(t.file("new.wav")));
        CHECK(!lib.assign("a.wav", 1).ok);
        CHECK(readBytes(t.file("index.txt")) == text);
        CHECK(readBytes(t.file("a.wav")) == wavBytes);

        // Once the index is v1 again, open() works.
        writeBytes(t.file("index.txt"), indexOf("a.wav\t4\t48\tAlpha\n"));
        CHECK(lib.open().ok);
        CHECK(lib.find("a.wav") != nullptr);
        if (lib.find("a.wav"))
            CHECK_EQ(lib.find("a.wav")->slot, 4);
    }
}

TEST(index_over_one_mebibyte_is_not_parsed)
{
    TempDir t;
    std::error_code ec;
    fs::create_directories(t.dir(), ec);
    CHECK(writeWav(t.file("a.wav"), ramp(10), 1));
    const std::size_t mib = 1u << 20;
    const std::string base = indexOf("a.wav\t3\t50\tAlpha\n");

    // Exactly 1 MiB (padded with blank lines) is still an index.
    const std::string exact = base + std::string(mib - base.size(), '\n');
    CHECK_EQ(exact.size(), mib);
    writeBytes(t.file("index.txt"), exact);
    {
        SampleLibrary lib(t.lib);
        CHECK(lib.open().ok);
        CHECK(lib.warnings().empty());
        CHECK(lib.find("a.wav") != nullptr);
        if (lib.find("a.wav"))
            CHECK_EQ(lib.find("a.wav")->slot, 3);
        CHECK(!fs::exists(t.file("index.txt.bad")));
    }

    // One byte more is treated as damaged: warning, backup, adoption from the folder.
    const std::string over = base + std::string(mib - base.size() + 1, '\n');
    writeBytes(t.file("index.txt"), over);
    SampleLibrary lib(t.lib);
    CHECK(lib.open().ok);
    CHECK(!lib.warnings().empty());
    CHECK(hasWarningWith(lib, "1 MiB"));
    CHECK_EQ(lib.entries().size(), 1u);
    CHECK(lib.find("a.wav") != nullptr);
    if (lib.find("a.wav"))
        CHECK_EQ(lib.find("a.wav")->slot, kNoSlot); // the huge file was not read
    CHECK(readBytes(t.file("index.txt.bad")) == over);
    CHECK(readBytes(t.file("index.txt")).size() < 200u);
}

TEST(windows_device_and_colon_names)
{
    TempDir t;
    SampleLibrary lib(t.lib);
    CHECK(lib.open().ok);
    for (const char* name : {"con.kit", "CON.kit", "aux.x.y", "Nul.", "com1.foo", "lpt9.a", "prn", "con", "com5"})
    {
        std::string f;
        CHECK(lib.add(name, 1, ramp(4), f).ok);
        std::string head = f.substr(0, f.find('.'));
        for (char& c : head)
            c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        CHECK(head != "con" && head != "prn" && head != "aux" && head != "nul");
        CHECK(!(head.size() == 4 && (head.compare(0, 3, "com") == 0 || head.compare(0, 3, "lpt") == 0)));
        CHECK(fs::exists(t.file(f)));
        CHECK(f.size() > 4 && f.compare(f.size() - 4, 4, ".wav") == 0);
    }
    std::string f;
    CHECK(lib.add("con.kit", 1, ramp(4), f).ok);
    CHECK(f.compare(0, 4, "con_") == 0);

    // A name with a colon in the index line is skipped on every platform.
    const std::string text = indexOf("C:foo.wav\t-\t60\tx\n");
    writeBytes(t.file("index.txt"), text);
    SampleLibrary other(t.lib);
    CHECK(other.open().ok);
    CHECK(other.find("C:foo.wav") == nullptr);
    CHECK(hasWarningWith(other, "line 2"));

    // And a sample whose name has a colon makes a file without one.
    std::string g;
    CHECK(other.add("a:b", 1, ramp(4), g).ok);
    CHECK(g.find(':') == std::string::npos);
}

TEST(same_file_under_another_spelling_is_not_adopted_twice)
{
    TempDir t;
    std::error_code ec;
    fs::create_directories(t.dir(), ec);
    const std::string nfc = "caf\xC3\xA9.wav";
    const std::string nfd = "cafe\xCC\x81.wav";
    CHECK(writeWav(t.file(nfd), ramp(10), 1)); // the disk spells it one way ...
    writeBytes(t.file("index.txt"), indexOf(nfc + "\t9\t60\tCafe\n")); // ... the index the other
    if (!fs::equivalent(t.file(nfc), t.file(nfd), ec) || ec)
        return; // this file system keeps them apart, so they are two files here

    SampleLibrary lib(t.lib);
    CHECK(lib.open().ok);
    CHECK_EQ(lib.entries().size(), 1u);
    CHECK(lib.find(nfc) != nullptr);
    if (lib.find(nfc))
    {
        CHECK_EQ(lib.find(nfc)->slot, 9);
        CHECK(!lib.find(nfc)->missing);
    }
    CHECK(lib.find(nfd) == nullptr);
}

TEST(unusable_wav_warning_says_why)
{
    TempDir t;
    std::error_code ec;
    fs::create_directories(t.dir(), ec);
    CHECK(writeWav(t.file("rate.wav"), ramp(30), 1, 44100));
    CHECK(writeWav(t.file("three.wav"), ramp(30), 3));
    writeBytes(t.file("junk.wav"), "this is not a wav file at all");
    SampleLibrary lib(t.lib);
    CHECK(lib.open().ok);
    CHECK(hasWarningWith(lib, "rate.wav: is 44100 Hz, not 48000 Hz"));
    CHECK(hasWarningWith(lib, "three.wav: has 3 channels"));
    CHECK(hasWarningWith(lib, "junk.wav: is not a 48 kHz float WAV"));
}

TEST(oversized_file_is_refused_without_being_loaded)
{
    TempDir t;
    std::error_code ec;
    fs::create_directories(t.dir(), ec);
    // 30 MB of zeros: bigger than the longest legal sample, and not a WAV. If it were loaded
    // the reason would be "not a 48 kHz float WAV" instead.
    const std::string big(30u * 1000u * 1000u, '\0');
    writeBytes(t.file("big.wav"), big);
    writeBytes(t.file("index.txt"), indexOf("listed.wav\t-\t60\tx\n"));
    writeBytes(t.file("listed.wav"), big);

    SampleLibrary lib(t.lib);
    CHECK(lib.open().ok);
    CHECK(lib.find("big.wav") == nullptr);
    CHECK(hasWarningWith(lib, "big.wav: is longer than 60 seconds"));
    CHECK(hasWarningWith(lib, "listed.wav: is longer than 60 seconds"));
    CHECK(lib.find("listed.wav") != nullptr && lib.find("listed.wav")->missing);
    CHECK_EQ(fs::file_size(t.file("big.wav"), ec), 30000000u);
    CHECK_EQ(fs::file_size(t.file("listed.wav"), ec), 30000000u);
}

TEST(failed_save_leaves_memory_unchanged)
{
    TempDir t;
    SampleLibrary lib(t.lib);
    CHECK(lib.open().ok);
    std::string a, b;
    CHECK(lib.add("kick", 1, ramp(20), a).ok);
    CHECK(lib.add("snare", 2, ramp(40), b).ok);
    CHECK(lib.assign(a, 5).ok);
    CHECK(lib.assign(b, 6).ok);
    const std::string idxBefore = readBytes(t.file("index.txt"));

    auto snapshot = [&]() { return lib.entries(); };
    auto sameAs = [&](const std::vector<LibraryEntry>& s)
    {
        if (s.size() != lib.entries().size())
            return false;
        for (std::size_t i = 0; i < s.size(); ++i)
        {
            if (!sameEntry(s[i], lib.entries()[i]))
                return false;
        }
        return true;
    };

    // A folder where the temporary file would go makes every save fail.
    std::error_code ec;
    CHECK(fs::create_directory(t.file("index.txt.tmp"), ec));
    const std::vector<LibraryEntry> before = snapshot();

    CHECK(!lib.assign(a, 9).ok);
    CHECK(sameAs(before));
    CHECK(!lib.assign(a, 6).ok); // would have displaced b
    CHECK(sameAs(before));
    CHECK(!lib.clearSlot(5).ok);
    CHECK(sameAs(before));
    CHECK(!lib.setRootNote(a, 33).ok);
    CHECK(sameAs(before));
    CHECK(!lib.rename(a, "renamed").ok);
    CHECK(sameAs(before));
    std::string c;
    CHECK(!lib.add("tom", 1, ramp(8), c).ok);
    CHECK(c.empty());
    CHECK(sameAs(before));
    CHECK(lib.inSlot(5) == lib.find(a));
    CHECK(lib.inSlot(6) == lib.find(b));

    CHECK(fs::is_directory(t.file("index.txt.tmp"))); // the folder was left alone
    CHECK(readBytes(t.file("index.txt")) == idxBefore);

    // With the folder gone the same calls work. (The WAV that add() wrote is still there.)
    fs::remove(t.file("index.txt.tmp"), ec);
    CHECK(lib.assign(a, 9).ok);
    CHECK(lib.find(a) != nullptr);
    if (lib.find(a))
        CHECK_EQ(lib.find(a)->slot, 9);
    CHECK(lib.add("tom", 1, ramp(8), c).ok);
    CHECK(c == "tom-2.wav" || c == "tom.wav");
}

TEST(first_free_slot_skips_a_slot_a_missing_entry_holds)
{
    TempDir t;
    std::string a, b;
    {
        SampleLibrary lib(t.lib);
        CHECK(lib.open().ok);
        CHECK(lib.add("a", 1, ramp(10), a).ok);
        CHECK(lib.add("b", 1, ramp(10), b).ok);
        CHECK(lib.assign(a, 0).ok);
        CHECK(lib.assign(b, 2).ok);
    }
    std::error_code ec;
    fs::remove(t.file(a), ec); // this test's own folder
    SampleLibrary lib(t.lib);
    CHECK(lib.open().ok);
    CHECK(lib.find(a) != nullptr && lib.find(a)->missing);
    if (lib.find(a))
        CHECK_EQ(lib.find(a)->slot, 0);
    CHECK_EQ(lib.firstFreeSlot(), 1); // not 0: the missing sample still holds it
    CHECK(lib.inSlot(0) == lib.find(a));
}

TEST(add_never_writes_into_an_existing_name)
{
#if !defined(_WIN32)
    // A dangling symlink named like the wanted file: add() must pick another name and must
    // not create the link's target.
    TempDir t;
    const fs::path target = t.root / "created_through_link.wav";
    std::error_code ec;
    fs::create_directories(t.dir(), ec);
    fs::create_symlink(target, t.file("kick.wav"), ec);
    CHECK(!ec);
    SampleLibrary lib(t.lib);
    CHECK(lib.open().ok);
    std::string f;
    CHECK(lib.add("kick", 1, ramp(10), f).ok);
    CHECK(f == "kick-2.wav");
    CHECK(!fs::exists(target));
    CHECK(fs::is_symlink(t.file("kick.wav")));
#endif
    TempDir t2;
    SampleLibrary lib2(t2.lib);
    CHECK(lib2.open().ok);
    std::string a, b;
    CHECK(lib2.add("kick", 1, ramp(10), a).ok);
    const std::string first = readBytes(t2.file(a));
    CHECK(lib2.add("kick", 1, ramp(12), b).ok);
    CHECK(a != b);
    CHECK(readBytes(t2.file(a)) == first);
    CHECK(!fs::exists(t2.file("index.txt.tmp")));
}

TEST(read_wav_float_refuses_a_chunk_size_that_would_wrap)
{
    TempDir t;
    std::error_code ec;
    fs::create_directories(t.dir(), ec);
    CHECK(writeWav(t.file("a.wav"), ramp(10), 1));
    std::string bytes = readBytes(t.file("a.wav"));
    // The data chunk size field (offset 40) set to nearly 4 GiB.
    for (int i = 0; i < 4; ++i)
        bytes[40 + static_cast<std::size_t>(i)] = static_cast<char>(i == 0 ? 0xF0 : 0xFF);
    writeBytes(t.file("b.wav"), bytes);
    std::vector<float> s;
    int ch = 0, rate = 0;
    CHECK(!readWav(t.file("b.wav"), s, ch, rate));
    CHECK(readWav(t.file("a.wav"), s, ch, rate));
}


PHYLO_TEST_MAIN
