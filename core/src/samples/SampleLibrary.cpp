#include "phylo/samples/SampleLibrary.h"

#include "phylo/Wav.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <utility>

namespace phylo::samples
{

namespace
{

namespace fs = std::filesystem;

constexpr char kIndexTempName[] = "index.txt.tmp";
constexpr char kIndexBadName[] = "index.txt.bad";
constexpr std::size_t kMaxStemChars = 48;

fs::path toPath(const std::string& utf8)
{
    return fs::u8path(utf8);
}

std::string toUtf8(const fs::path& p)
{
    return p.u8string();
}

fs::path filePath(const std::string& folder, const std::string& file)
{
    return toPath(folder) / toPath(file);
}

char lowerAscii(char c)
{
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string lowered(const std::string& s)
{
    std::string r = s;
    for (char& c : r)
        c = lowerAscii(c);
    return r;
}

bool isReservedFileName(const std::string& file)
{
    const std::string l = lowered(file);
    return l == kIndexFileName || l == kIndexTempName || l == kIndexBadName;
}

// A file name from the index must stay inside the folder and not be one of our own files.
bool isSafeFileName(const std::string& file)
{
    if (file.empty() || file == "." || file == "..")
        return false;
    if (file.find('/') != std::string::npos || file.find('\\') != std::string::npos)
        return false;
    for (char c : file)
    {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F)
            return false;
    }
    return !isReservedFileName(file);
}

// Names are cleaned the same way the blob requires, so what is stored can always be sent.
std::string cleanName(const std::string& name)
{
    return sanitiseSampleName(name);
}

// Windows refuses files named after old device names, whatever the extension.
bool isDeviceName(const std::string& stem)
{
    const std::string l = lowered(stem);
    if (l == "con" || l == "prn" || l == "aux" || l == "nul")
        return true;
    return l.size() == 4 && (l.compare(0, 3, "com") == 0 || l.compare(0, 3, "lpt") == 0) &&
           l[3] >= '1' && l[3] <= '9';
}

std::string makeStem(const std::string& cleaned)
{
    std::string stem;
    for (char c : cleaned)
    {
        const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                          c == '-' || c == '_' || c == '.';
        stem.push_back(keep ? c : '_'); // space, separators and non-ASCII bytes all become '_'
    }
    if (stem.find_first_not_of('.') == std::string::npos)
        stem = "sample";
    if (stem.size() > kMaxStemChars)
        stem.resize(kMaxStemChars);
    if (isDeviceName(stem))
        stem.push_back('_');
    return stem;
}

bool isFiniteBits(float f)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof bits);
    return (bits & 0x7F800000u) != 0x7F800000u;
}

bool allFinite(const std::vector<float>& v)
{
    for (float f : v)
    {
        if (!isFiniteBits(f))
            return false;
    }
    return true;
}

// Loads a sample file and applies the usable-sample rule. On failure `why` says what is wrong.
bool loadUsable(const fs::path& path, std::vector<float>& samples, int& channels, std::string& why)
{
    std::error_code ec;
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec)
    {
        why = "cannot be read";
        return false;
    }
    // 60 s of stereo float, plus generous room for header and extra chunks.
    const std::uintmax_t limit = static_cast<std::uintmax_t>(kMaxSampleFrames) * kMaxChannels * 4 + (1u << 20);
    if (size > limit)
    {
        why = "is longer than 60 seconds";
        return false;
    }

    std::vector<float> data;
    int ch = 0;
    int rate = 0;
    if (!readWavFloat(toUtf8(path), data, ch, rate))
    {
        why = "is not a 48 kHz float WAV";
        return false;
    }
    if (rate != static_cast<int>(kSampleRateHz))
    {
        why = "is " + std::to_string(rate) + " Hz, not 48000 Hz";
        return false;
    }
    if (ch < 1 || ch > kMaxChannels)
    {
        why = "has " + std::to_string(ch) + " channels, not 1 or 2";
        return false;
    }
    if (data.empty() || data.size() % static_cast<std::size_t>(ch) != 0)
    {
        why = "has no audio";
        return false;
    }
    if (data.size() / static_cast<std::size_t>(ch) > kMaxSampleFrames)
    {
        why = "is longer than 60 seconds";
        return false;
    }
    if (!allFinite(data))
    {
        why = "contains values that are not finite numbers";
        return false;
    }
    samples = std::move(data);
    channels = ch;
    return true;
}

bool parseNumber(const std::string& s, int maxValue, int& out)
{
    if (s.empty() || s.size() > 3)
        return false;
    int v = 0;
    for (char c : s)
    {
        if (c < '0' || c > '9')
            return false;
        v = v * 10 + (c - '0');
    }
    if (v > maxValue)
        return false;
    out = v;
    return true;
}

std::vector<std::string> splitTabs(const std::string& line)
{
    std::vector<std::string> fields;
    std::size_t start = 0;
    for (;;)
    {
        const std::size_t tab = line.find('\t', start);
        if (tab == std::string::npos)
        {
            fields.push_back(line.substr(start));
            return fields;
        }
        fields.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
}

bool hasWavExtension(const fs::path& p)
{
    return lowered(toUtf8(p.extension())) == ".wav";
}

} // namespace

SampleLibrary::SampleLibrary(std::string folder) : folder_(std::move(folder)) {}

LibraryResult SampleLibrary::open()
{
    entries_.clear();
    warnings_.clear();
    indexDamaged_ = false;

    try
    {
        std::error_code ec;
        const fs::path dir = toPath(folder_);
        fs::create_directories(dir, ec);
        if (!fs::is_directory(dir, ec))
            return LibraryResult::failure("cannot create the sample folder " + folder_);

        // Read the index, if there is one.
        const fs::path indexPath = dir / kIndexFileName;
        bool haveIndex = fs::exists(indexPath, ec);
        if (haveIndex)
        {
            std::ifstream in(indexPath, std::ios::binary);
            if (!in)
            {
                warnings_.push_back("the index cannot be read; it will be kept as a .bad copy");
                indexDamaged_ = true;
            }
            else
            {
                const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                std::vector<std::string> lines;
                std::size_t start = 0;
                while (start < text.size())
                {
                    std::size_t end = text.find('\n', start);
                    if (end == std::string::npos)
                        end = text.size();
                    std::string line = text.substr(start, end - start);
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();
                    lines.push_back(std::move(line));
                    start = end + 1;
                }

                if (lines.empty() || lines[0] != kIndexHeader)
                {
                    warnings_.push_back("the index has an unknown first line and was not read");
                    indexDamaged_ = true;
                }
                else
                {
                    std::set<std::string> seenFiles;
                    std::array<bool, kSlotCount> slotTaken{};
                    for (std::size_t i = 1; i < lines.size(); ++i)
                    {
                        const std::string& line = lines[i];
                        if (line.empty())
                            continue;
                        const std::string where = "index line " + std::to_string(i + 1) + ": ";

                        const std::vector<std::string> f = splitTabs(line);
                        if (f.size() != 4)
                        {
                            warnings_.push_back(where + "expected 4 fields; line skipped");
                            indexDamaged_ = true;
                            continue;
                        }
                        int slot = kNoSlot;
                        if (f[1] != "-" && !parseNumber(f[1], kMaxSlot, slot))
                        {
                            warnings_.push_back(where + "slot must be - or 0 to 127; line skipped");
                            indexDamaged_ = true;
                            continue;
                        }
                        int root = 0;
                        if (!parseNumber(f[2], kMaxRootNote, root))
                        {
                            warnings_.push_back(where + "root note must be 0 to 127; line skipped");
                            indexDamaged_ = true;
                            continue;
                        }
                        if (!isSafeFileName(f[0]))
                        {
                            warnings_.push_back(where + "file name is not allowed; line skipped");
                            indexDamaged_ = true;
                            continue;
                        }
                        if (!seenFiles.insert(lowered(f[0])).second)
                        {
                            warnings_.push_back(where + "file " + f[0] + " is listed twice; line skipped");
                            indexDamaged_ = true;
                            continue;
                        }
                        if (slot != kNoSlot)
                        {
                            if (slotTaken[static_cast<std::size_t>(slot)])
                            {
                                warnings_.push_back(where + "slot " + std::to_string(slot) +
                                                    " is already taken; " + f[0] + " is unassigned");
                                indexDamaged_ = true;
                                slot = kNoSlot;
                            }
                            else
                            {
                                slotTaken[static_cast<std::size_t>(slot)] = true;
                            }
                        }

                        LibraryEntry e;
                        e.file = f[0];
                        e.name = cleanName(f[3]);
                        e.slot = slot;
                        e.rootNote = static_cast<std::uint8_t>(root);
                        entries_.push_back(std::move(e));
                    }
                }
            }
        }

        // Reconcile the index with the files that are there.
        for (LibraryEntry& e : entries_)
        {
            const fs::path p = dir / toPath(e.file);
            std::error_code fe;
            if (!fs::exists(p, fe))
            {
                e.missing = true;
                continue;
            }
            std::vector<float> samples;
            int ch = 0;
            std::string why;
            if (!fs::is_regular_file(p, fe))
            {
                e.missing = true;
                warnings_.push_back(e.file + ": is not a file");
            }
            else if (!loadUsable(p, samples, ch, why))
            {
                e.missing = true;
                warnings_.push_back(e.file + ": " + why + "; it will not be sent");
            }
            else
            {
                e.channels = ch;
                e.frames = static_cast<std::uint32_t>(samples.size() / static_cast<std::size_t>(ch));
            }
        }

        // Adopt WAV files that were put in the folder by hand, in sorted file-name order.
        std::set<std::string> known;
        for (const LibraryEntry& e : entries_)
            known.insert(lowered(e.file));

        std::vector<std::string> candidates;
        fs::directory_iterator it(dir, ec);
        const fs::directory_iterator endIt;
        for (; !ec && it != endIt; it.increment(ec))
        {
            std::error_code te;
            if (!it->is_regular_file(te) || !hasWavExtension(it->path()))
                continue;
            const std::string name = toUtf8(it->path().filename());
            if (isReservedFileName(name) || known.count(lowered(name)) != 0)
                continue;
            candidates.push_back(name);
        }
        std::sort(candidates.begin(), candidates.end());

        bool adopted = false;
        for (const std::string& name : candidates)
        {
            std::vector<float> samples;
            int ch = 0;
            std::string why;
            if (!loadUsable(dir / toPath(name), samples, ch, why))
            {
                warnings_.push_back(name + ": not a 48 kHz float WAV; import it through the app");
                continue;
            }
            LibraryEntry e;
            e.file = name;
            e.name = cleanName(toUtf8(toPath(name).stem()));
            e.channels = ch;
            e.frames = static_cast<std::uint32_t>(samples.size() / static_cast<std::size_t>(ch));
            entries_.push_back(std::move(e));
            adopted = true;
        }

        if (adopted || indexDamaged_)
        {
            const LibraryResult saved = saveIndex();
            if (!saved.ok)
                warnings_.push_back("the index was not saved: " + saved.message);
        }
    }
    catch (...)
    {
        return LibraryResult::failure("unexpected error while opening the sample folder " + folder_);
    }
    return {};
}

LibraryResult SampleLibrary::saveIndex()
{
    try
    {
        std::string text = kIndexHeader;
        text += '\n';
        for (const LibraryEntry& e : entries_)
        {
            text += e.file;
            text += '\t';
            text += e.slot == kNoSlot ? std::string("-") : std::to_string(e.slot);
            text += '\t';
            text += std::to_string(static_cast<int>(e.rootNote));
            text += '\t';
            text += cleanName(e.name);
            text += '\n';
        }

        const fs::path dir = toPath(folder_);
        const fs::path indexPath = dir / kIndexFileName;
        const fs::path tmpPath = dir / kIndexTempName;
        std::error_code ec;

        {
            std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
            if (out)
            {
                out.write(text.data(), static_cast<std::streamsize>(text.size()));
                out.flush();
            }
            if (!out)
            {
                out.close();
                fs::remove(tmpPath, ec); // our own temporary file, never a sample
                return LibraryResult::failure("cannot write the sample index in " + folder_);
            }
        }

        if (indexDamaged_ && fs::exists(indexPath, ec))
        {
            fs::copy_file(indexPath, dir / kIndexBadName, fs::copy_options::overwrite_existing, ec);
            if (ec)
            {
                fs::remove(tmpPath, ec);
                return LibraryResult::failure("cannot keep a copy of the damaged index (index.txt.bad)");
            }
        }

        fs::rename(tmpPath, indexPath, ec);
        if (ec)
        {
            std::error_code rc;
            fs::remove(tmpPath, rc);
            return LibraryResult::failure("cannot replace the sample index: " + ec.message());
        }
        indexDamaged_ = false;
        return {};
    }
    catch (...)
    {
        return LibraryResult::failure("unexpected error while saving the sample index");
    }
}

LibraryResult SampleLibrary::add(const std::string& name, int channels, const std::vector<float>& samples,
                                 std::string& fileOut)
{
    fileOut.clear();
    try
    {
        if (channels < 1 || channels > kMaxChannels)
            return LibraryResult::failure("a sample must be mono or stereo, not " + std::to_string(channels) +
                                          " channels");
        if (samples.empty())
            return LibraryResult::failure("the sample has no audio");
        if (samples.size() % static_cast<std::size_t>(channels) != 0)
            return LibraryResult::failure("the sample data does not fit the channel count");
        if (samples.size() / static_cast<std::size_t>(channels) > kMaxSampleFrames)
            return LibraryResult::failure("the sample is longer than 60 seconds");
        if (!allFinite(samples))
            return LibraryResult::failure("the sample contains values that are not finite numbers");

        const fs::path dir = toPath(folder_);
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (!fs::is_directory(dir, ec))
            return LibraryResult::failure("cannot create the sample folder " + folder_);

        // Names already in use, compared without case: macOS and Windows ignore it.
        std::set<std::string> taken;
        for (const LibraryEntry& e : entries_)
            taken.insert(lowered(e.file));
        fs::directory_iterator it(dir, ec);
        const fs::directory_iterator endIt;
        for (; !ec && it != endIt; it.increment(ec))
            taken.insert(lowered(toUtf8(it->path().filename())));

        const std::string cleaned = cleanName(name);
        const std::string stem = makeStem(cleaned);
        std::string file;
        for (int n = 1;; ++n)
        {
            file = n == 1 ? stem + ".wav" : stem + "-" + std::to_string(n) + ".wav";
            std::error_code xe;
            if (taken.count(lowered(file)) == 0 && !fs::exists(dir / toPath(file), xe))
                break;
        }

        const fs::path target = dir / toPath(file);
        if (!writeWavFloat(toUtf8(target), samples, channels, static_cast<int>(kSampleRateHz)))
        {
            std::error_code re;
            fs::remove(target, re); // the partial file this call just created
            return LibraryResult::failure("cannot write " + file);
        }

        LibraryEntry e;
        e.file = file;
        e.name = cleaned;
        e.channels = channels;
        e.frames = static_cast<std::uint32_t>(samples.size() / static_cast<std::size_t>(channels));
        entries_.push_back(std::move(e));

        const LibraryResult saved = saveIndex();
        if (!saved.ok)
        {
            // The WAV stays in the folder (nothing here deletes a sample); the next open() adopts it.
            entries_.pop_back();
            return LibraryResult::failure(saved.message + " (" + file + " was written and will be adopted on the next open)");
        }
        fileOut = file;
        return {};
    }
    catch (...)
    {
        return LibraryResult::failure("unexpected error while adding the sample");
    }
}

LibraryResult SampleLibrary::assign(const std::string& file, int slot)
{
    if (slot < 0 || slot > kMaxSlot)
        return LibraryResult::failure("slot " + std::to_string(slot) + " is out of range (0 to 127)");
    LibraryEntry* target = nullptr;
    for (LibraryEntry& e : entries_)
    {
        if (e.file == file)
            target = &e;
    }
    if (target == nullptr)
        return LibraryResult::failure(file + " is not in the sample library");
    if (target->missing)
        return LibraryResult::failure(file + " is missing from the sample folder");
    if (target->slot == slot)
        return {};

    const std::vector<LibraryEntry> backup = entries_;
    for (LibraryEntry& e : entries_)
    {
        if (e.slot == slot)
            e.slot = kNoSlot;
    }
    target->slot = slot;

    const LibraryResult saved = saveIndex();
    if (!saved.ok)
        entries_ = backup;
    return saved;
}

LibraryResult SampleLibrary::clearSlot(int slot)
{
    if (slot < 0 || slot > kMaxSlot)
        return LibraryResult::failure("slot " + std::to_string(slot) + " is out of range (0 to 127)");
    const std::vector<LibraryEntry> backup = entries_;
    bool changed = false;
    for (LibraryEntry& e : entries_)
    {
        if (e.slot == slot)
        {
            e.slot = kNoSlot;
            changed = true;
        }
    }
    if (!changed)
        return {};

    const LibraryResult saved = saveIndex();
    if (!saved.ok)
        entries_ = backup;
    return saved;
}

LibraryResult SampleLibrary::setRootNote(const std::string& file, int rootNote)
{
    if (rootNote < 0 || rootNote > kMaxRootNote)
        return LibraryResult::failure("root note " + std::to_string(rootNote) + " is out of range (0 to 127)");
    for (LibraryEntry& e : entries_)
    {
        if (e.file != file)
            continue;
        const std::uint8_t old = e.rootNote;
        e.rootNote = static_cast<std::uint8_t>(rootNote);
        const LibraryResult saved = saveIndex();
        if (!saved.ok)
            e.rootNote = old;
        return saved;
    }
    return LibraryResult::failure(file + " is not in the sample library");
}

LibraryResult SampleLibrary::rename(const std::string& file, const std::string& name)
{
    for (LibraryEntry& e : entries_)
    {
        if (e.file != file)
            continue;
        const std::string old = e.name;
        e.name = cleanName(name);
        const LibraryResult saved = saveIndex();
        if (!saved.ok)
            e.name = old;
        return saved;
    }
    return LibraryResult::failure(file + " is not in the sample library");
}

const LibraryEntry* SampleLibrary::find(const std::string& file) const noexcept
{
    for (const LibraryEntry& e : entries_)
    {
        if (e.file == file)
            return &e;
    }
    return nullptr;
}

const LibraryEntry* SampleLibrary::inSlot(int slot) const noexcept
{
    if (slot < 0 || slot > kMaxSlot)
        return nullptr;
    for (const LibraryEntry& e : entries_)
    {
        if (e.slot == slot)
            return &e;
    }
    return nullptr;
}

int SampleLibrary::firstFreeSlot() const noexcept
{
    std::array<bool, kSlotCount> used{};
    for (const LibraryEntry& e : entries_)
    {
        if (e.slot >= 0 && e.slot <= kMaxSlot)
            used[static_cast<std::size_t>(e.slot)] = true;
    }
    for (int i = 0; i < kSlotCount; ++i)
    {
        if (!used[static_cast<std::size_t>(i)])
            return i;
    }
    return kNoSlot;
}

LibraryResult SampleLibrary::buildSlotSet(SampleSlotSet& out) const
{
    out.clear();
    try
    {
        std::vector<const LibraryEntry*> assigned;
        for (const LibraryEntry& e : entries_)
        {
            if (e.slot != kNoSlot && !e.missing)
                assigned.push_back(&e);
        }
        std::sort(assigned.begin(), assigned.end(),
                  [](const LibraryEntry* a, const LibraryEntry* b) { return a->slot < b->slot; });

        SampleSlotSet result;
        for (const LibraryEntry* e : assigned)
        {
            SampleSlot s;
            int ch = 0;
            std::string why;
            if (!loadUsable(filePath(folder_, e->file), s.samples, ch, why))
                return LibraryResult::failure(e->file + " " + why);
            s.slot = e->slot;
            s.name = e->name;
            s.rootNote = e->rootNote;
            s.channels = ch;
            result.push_back(std::move(s));
        }
        out = std::move(result);
        return {};
    }
    catch (...)
    {
        out.clear();
        return LibraryResult::failure("unexpected error while loading the samples");
    }
}

} // namespace phylo::samples
