// The app's sample library session: import, slots, and the blob to send (no JUCE).
//
// Ties the core pieces together for the UI: phylo/samples/SampleImport.h reads and converts
// a dropped WAV, SampleLibrary.h stores it and keeps the slots, SampleBlob.h encodes the
// slot set. Kept free of JUCE so it can be checked headlessly.
//
// Not thread-safe. The app uses it from the message thread only.
#pragma once

#include "phylo/samples/SampleBlob.h"
#include "phylo/samples/SampleLibrary.h"

#include <cstdint>
#include <string>
#include <vector>

// Test hook for the refusal check (docs/HANDOFF.md 8.1). When this environment variable is
// set to header, rate, slot, length or truncated, the app damages the blob it sends for a
// slot change that way, so the refusal can be seen on the status line. The blob sent when
// the host starts is left good, so there are accepted slots to keep playing.
constexpr char kSampleFaultEnv[] = "PHYLO_SAMPLE_FAULT";

class SampleSession
{
public:
    // `folder` is a UTF-8 path, in the app Documents/Phylotone/samples.
    explicit SampleSession(std::string folder);

    // Opens (or re-reads) the library. Returns the reason on failure.
    phylo::samples::LibraryResult open();

    const phylo::samples::SampleLibrary& library() const noexcept { return library_; }

    // Imports a WAV: reads it, converts it to 48 kHz float, refuses it if it is longer than
    // 60 s or not a supported file, and copies the result into the library under the source
    // file's name. The source file is only read. `fileOut` receives the library file name.
    phylo::samples::LibraryResult importFile(const std::string& sourcePath, std::string& fileOut);

    phylo::samples::LibraryResult assign(const std::string& file, int slot);
    phylo::samples::LibraryResult clearSlot(int slot);
    phylo::samples::LibraryResult setRootNote(const std::string& file, int rootNote);

    // Builds the blob for the current slots. Fails with the reason when a file cannot be
    // read or the set is over the size budget; `blob` is then empty. `slotsOut` receives the
    // number of slots in the blob. `fault` is the value of kSampleFaultEnv, or null.
    phylo::samples::LibraryResult buildBlob(std::vector<std::uint8_t>& blob, int& slotsOut,
                                            const char* fault) const;

    // How many slots hold a sample that is present on disk.
    int assignedSlots() const noexcept;

private:
    phylo::samples::SampleLibrary library_;
};
