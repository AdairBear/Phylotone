#include "SampleSession.h"

#include "phylo/samples/SampleImport.h"

#include <cstring>
#include <filesystem>
#include <utility>

using phylo::samples::LibraryResult;

bool isKnownSampleFaultWord(const char* word)
{
    if (word == nullptr)
        return false;
    for (const char* known : {"header", "rate", "slot", "length", "truncated"})
        if (std::strcmp(word, known) == 0)
            return true;
    return false;
}

SampleSession::SampleSession(std::string folder) : library_(std::move(folder)) {}

LibraryResult SampleSession::open()
{
    return library_.open();
}

LibraryResult SampleSession::importFile(const std::string& sourcePath, std::string& fileOut)
{
    const auto path = std::filesystem::u8path(sourcePath);
    const std::string shown = path.filename().u8string();

    phylo::samples::ImportedSample sample;
    const auto imported = phylo::samples::importWavFile(sourcePath, sample);
    if (!imported.ok())
        return LibraryResult::failure(shown + ": " + imported.message);

    const auto added = library_.add(path.stem().u8string(), sample.channels, sample.samples, fileOut);
    if (!added.ok)
        return LibraryResult::failure(shown + ": " + added.message);
    return added;
}

LibraryResult SampleSession::assign(const std::string& file, int slot)
{
    return library_.assign(file, slot);
}

LibraryResult SampleSession::clearSlot(int slot)
{
    return library_.clearSlot(slot);
}

LibraryResult SampleSession::setRootNote(const std::string& file, int rootNote)
{
    return library_.setRootNote(file, rootNote);
}

LibraryResult SampleSession::buildBlob(std::vector<std::uint8_t>& blob, int& slotsOut, const char* fault) const
{
    using namespace phylo::samples;

    blob.clear();
    slotsOut = 0;

    SampleSlotSet set;
    const auto built = library_.buildSlotSet(set);
    if (!built.ok)
        return built;

    const auto encoded = encodeSampleBlob(set, blob);
    if (!encoded.ok())
    {
        blob.clear();
        return LibraryResult::failure(encoded.message);
    }
    slotsOut = static_cast<int>(set.size());

    // Test hook. The damaged blob is deliberately not checked here: the host must refuse it.
    if (fault != nullptr && fault[0] != '\0')
    {
        BlobFault kind = BlobFault::WrongHeader;
        bool known = true;
        if (std::strcmp(fault, "header") == 0)
            kind = BlobFault::WrongHeader;
        else if (std::strcmp(fault, "rate") == 0)
            kind = BlobFault::WrongSampleRate;
        else if (std::strcmp(fault, "slot") == 0)
            kind = BlobFault::SlotOutOfRange;
        else if (std::strcmp(fault, "length") == 0)
            kind = BlobFault::SampleTooLong;
        else if (std::strcmp(fault, "truncated") == 0)
            kind = BlobFault::Truncated;
        else
            known = false; // an unknown word is ignored
        // Slot and length faults need a slot to damage. With none, damage the blob another way,
        // so that a known fault word always sends a blob the host must refuse.
        if (known && !applyBlobFaultForTesting(blob, kind))
            applyBlobFaultForTesting(blob, BlobFault::Truncated);
    }
    return {};
}

int SampleSession::assignedSlots() const noexcept
{
    int n = 0;
    for (const auto& e : library_.entries())
        if (e.slot != phylo::samples::kNoSlot && !e.missing)
            ++n;
    return n;
}
