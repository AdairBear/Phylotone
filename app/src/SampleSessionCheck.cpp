// Headless check of the sample session (docs/HANDOFF.md 8.1). Plain C++, no JUCE.
//
// Walks the acceptance path in a fresh temporary folder: import a WAV, see it in the
// library, assign a slot, build the blob, restart, change the root note, clear the slot.
// Then the refusals and the fault hook.
#include "SampleSession.h"

#include "phylo/Wav.h"
#include "phylo/samples/SampleBlob.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace phylo::samples;

namespace
{
int failures = 0;

#define CHECK(cond)                                                             \
    do                                                                          \
    {                                                                           \
        if (!(cond))                                                            \
        {                                                                       \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                 \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

void put16(std::string& s, unsigned v)
{
    s.push_back(static_cast<char>(v & 0xff));
    s.push_back(static_cast<char>((v >> 8) & 0xff));
}

void put32(std::string& s, std::uint32_t v)
{
    put16(s, v & 0xffff);
    put16(s, v >> 16);
}

// A 16-bit PCM WAV, written by hand (the library writer only does float).
bool writeWav16(const fs::path& path, int channels, int rate, std::size_t frames)
{
    const std::uint32_t dataBytes = static_cast<std::uint32_t>(frames * static_cast<std::size_t>(channels) * 2);
    std::string b = "RIFF";
    put32(b, 36 + dataBytes);
    b += "WAVEfmt ";
    put32(b, 16);
    put16(b, 1);
    put16(b, static_cast<unsigned>(channels));
    put32(b, static_cast<std::uint32_t>(rate));
    put32(b, static_cast<std::uint32_t>(rate * channels * 2));
    put16(b, static_cast<unsigned>(channels * 2));
    put16(b, 16);
    b += "data";
    put32(b, dataBytes);
    for (std::size_t i = 0; i < frames * static_cast<std::size_t>(channels); ++i)
        put16(b, static_cast<unsigned>(static_cast<int>(8000.0 * std::sin(static_cast<double>(i) * 0.05)) & 0xffff));
    std::ofstream out(path, std::ios::binary);
    out.write(b.data(), static_cast<std::streamsize>(b.size()));
    return static_cast<bool>(out);
}

std::string readAll(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::size_t countFiles(const fs::path& dir)
{
    std::size_t n = 0;
    for (const auto& e : fs::directory_iterator(dir))
        if (e.is_regular_file())
            ++n;
    return n;
}

bool decode(const std::vector<std::uint8_t>& blob, SampleSlotSet& out)
{
    return decodeSampleBlob(blob.data(), blob.size(), out).ok();
}

// Builds a blob with the given fault word and returns what checkSampleBlob says about it.
BlobError faultResult(const SampleSession& s, const char* fault, bool& built)
{
    std::vector<std::uint8_t> blob;
    int slots = 0;
    built = s.buildBlob(blob, slots, fault).ok;
    return checkSampleBlob(blob.data(), blob.size()).code;
}
} // namespace

int main()
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() / ("phylo_sample_session_" + std::to_string(stamp));
    const fs::path source = root / "source";
    const fs::path library = root / "library";
    fs::create_directories(source);

    const fs::path kick = source / "kick.wav";
    const fs::path tone = source / "tone.wav";
    CHECK(writeWav16(kick, 2, 44100, 4410)); // 0.1 s stereo, 44.1 kHz
    {
        std::vector<float> v(4800);
        for (std::size_t i = 0; i < v.size(); ++i)
            v[i] = 0.25f * static_cast<float>(std::sin(static_cast<double>(i) * 0.1));
        CHECK(phylo::writeWavFloat(tone.string(), v, 1, 48000));
    }
    const std::string kickBefore = readAll(kick);

    // Open, import, see it unassigned.
    std::string kickFile;
    {
        SampleSession s(library.u8string());
        CHECK(s.open().ok);
        CHECK(s.library().entries().empty());

        const auto imported = s.importFile(kick.u8string(), kickFile);
        CHECK(imported.ok);
        CHECK(s.library().entries().size() == 1);
        const auto* e = s.library().find(kickFile);
        CHECK(e != nullptr);
        if (e != nullptr)
        {
            CHECK(e->slot == kNoSlot);
            CHECK(e->name == "kick");
            CHECK(e->channels == 2);
            CHECK(e->frames == 4800);
            CHECK(e->rootNote == kDefaultRootNote);
        }
        CHECK(fs::exists(library / kickFile));
        CHECK(readAll(kick) == kickBefore); // the source is untouched
        CHECK(s.assignedSlots() == 0);

        // Assign slot 0 and build.
        CHECK(s.assign(kickFile, 0).ok);
        CHECK(s.assignedSlots() == 1);
        std::vector<std::uint8_t> blob;
        int slots = -1;
        CHECK(s.buildBlob(blob, slots, nullptr).ok);
        CHECK(slots == 1);
        SampleSlotSet set;
        CHECK(decode(blob, set));
        CHECK(set.size() == 1);
        if (set.size() == 1)
        {
            CHECK(set[0].slot == 0);
            CHECK(set[0].name == "kick");
            CHECK(set[0].channels == 2);
            CHECK(set[0].frames() == 4800);
            CHECK(set[0].rootNote == 60);
        }
    }

    // A restart on the same folder still has the sample in slot 0 with its name.
    {
        SampleSession s(library.u8string());
        CHECK(s.open().ok);
        const auto* e = s.library().inSlot(0);
        CHECK(e != nullptr);
        if (e != nullptr)
        {
            CHECK(e->file == kickFile);
            CHECK(e->name == "kick");
        }
        CHECK(s.assignedSlots() == 1);

        // The root note shows up in the blob.
        CHECK(s.setRootNote(kickFile, 48).ok);
        std::vector<std::uint8_t> blob;
        int slots = 0;
        SampleSlotSet set;
        CHECK(s.buildBlob(blob, slots, nullptr).ok);
        CHECK(decode(blob, set));
        CHECK(set.size() == 1 && set[0].rootNote == 48);

        // Clearing the slot gives a valid blob with no slots; the sample stays in the library.
        CHECK(s.clearSlot(0).ok);
        CHECK(s.buildBlob(blob, slots, nullptr).ok);
        CHECK(slots == 0);
        CHECK(checkSampleBlob(blob.data(), blob.size()).ok());
        CHECK(decode(blob, set));
        CHECK(set.empty());
        CHECK(s.library().entries().size() == 1);
        CHECK(s.assignedSlots() == 0);
    }

    // Refusals leave the library and the folder as they were.
    {
        SampleSession s(library.u8string());
        CHECK(s.open().ok);
        const auto entriesBefore = s.library().entries().size();
        const auto filesBefore = countFiles(library);
        std::string out;

        const fs::path longFile = source / "long.wav";
        CHECK(writeWav16(longFile, 1, 8000, 488000)); // 61 s
        const auto tooLong = s.importFile(longFile.u8string(), out);
        CHECK(!tooLong.ok);
        CHECK(tooLong.message.find("60 s") != std::string::npos);
        CHECK(tooLong.message.find("long.wav") != std::string::npos);

        const fs::path text = source / "notes.wav";
        {
            std::ofstream f(text, std::ios::binary);
            f << "this is not a wav file at all, just some text";
        }
        CHECK(!s.importFile(text.u8string(), out).ok);

        const auto missing = s.importFile((source / "nothing.wav").u8string(), out);
        CHECK(!missing.ok);
        CHECK(!missing.message.empty());

        CHECK(s.library().entries().size() == entriesBefore);
        CHECK(countFiles(library) == filesBefore);
    }

    // The fault hook: the blob is built and damaged, not validated by the session.
    {
        SampleSession s(library.u8string());
        CHECK(s.open().ok);
        std::string toneFile;
        CHECK(s.importFile(tone.u8string(), toneFile).ok);
        CHECK(s.assign(toneFile, 5).ok);

        struct Case
        {
            const char* word;
            BlobError expected;
        };
        const Case cases[] = {
            { "header", BlobError::UnknownHeader },   { "rate", BlobError::WrongSampleRate },
            { "slot", BlobError::SlotOutOfRange },    { "length", BlobError::SampleTooLong },
            { "truncated", BlobError::Truncated },
        };
        for (const auto& c : cases)
        {
            bool built = false;
            const auto got = faultResult(s, c.word, built);
            CHECK(built);
            CHECK(got == c.expected);
        }

        bool built = false;
        CHECK(faultResult(s, "nonsense", built) == BlobError::None);
        CHECK(built);
        CHECK(faultResult(s, "", built) == BlobError::None);
        CHECK(built);
        CHECK(faultResult(s, nullptr, built) == BlobError::None);
        CHECK(built);

        // An assigned file that disappears after open: the library reads it when the blob
        // is built and fails naming the file (SampleLibrary::buildSlotSet).
        fs::remove(library / toneFile);
        std::vector<std::uint8_t> blob;
        int slots = 0;
        const auto failed = s.buildBlob(blob, slots, nullptr);
        CHECK(!failed.ok);
        CHECK(failed.message.find(toneFile) != std::string::npos);
        CHECK(blob.empty());

        // After a re-open it is marked missing, keeps its slot and is skipped.
        CHECK(s.open().ok);
        CHECK(s.assignedSlots() == 0);
        CHECK(s.buildBlob(blob, slots, nullptr).ok);
        CHECK(slots == 0);

        CHECK(isKnownSampleFaultWord("slot"));
        CHECK(!isKnownSampleFaultWord("nonsense"));
        CHECK(!isKnownSampleFaultWord(""));
        CHECK(!isKnownSampleFaultWord(nullptr));

        // A fault that needs a slot, asked for on a zero-slot blob, still damages the blob.
        for (const char* word : {"slot", "length"})
        {
            bool zeroBuilt = false;
            CHECK(faultResult(s, word, zeroBuilt) != BlobError::None);
            CHECK(zeroBuilt);
        }
    }

    std::error_code ec;
    fs::remove_all(root, ec);

    if (failures == 0)
        std::printf("sample session check: all passed\n");
    else
        std::printf("sample session check: %d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
