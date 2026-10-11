// The sample library: a folder with one 48 kHz float WAV per sample, and an index file so
// a sample keeps its name, slot and root note across restarts. JUCE-free (std::filesystem).
//
// The app uses Documents/Phylotone/samples. Rules:
//  - Copy-in only. Adding a sample writes a new file in the folder. Nothing here deletes or
//    overwrites a sample file, in the folder or outside it. A new file never takes the name
//    of a file that is already there.
//  - The index is `index.txt` in the folder. It is written to a temporary file and renamed
//    into place, so a crash during a save leaves the old index, not half of a new one.
//  - The index is only ever written through plain files. If `index.txt` or `index.txt.tmp`
//    is a symlink or a folder, nothing is written through it and the save fails with a
//    message naming the file; the symlink or folder is left alone.
//  - An index that cannot be fully read (damaged lines, an unknown first line, a file over
//    1 MiB) is copied to `index.txt.bad` before it is replaced. The copy never overwrites an
//    earlier one: the next is `index.txt.bad-2`, then `-3`, and so on. A UTF-8 byte order
//    mark before the first line is ignored.
//  - An index whose first line starts with `phylotone.samples.index.` but is not exactly
//    `...v1` came from a newer version. open() fails with a message, leaves the file
//    untouched and the library empty, and nothing in this object saves until the next open().
//  - A file listed in the index that is missing from the folder stays in the index, marked
//    missing, and keeps its slot. It is not sent. If the file comes back, so does the slot.
//  - A WAV placed in the folder by hand is adopted on open (unassigned) when it is already
//    48 kHz float, mono or stereo, 1 frame to 60 s. Any other file is left alone and
//    reported in warnings(). So is a file whose name the index cannot hold (a control
//    character, a backslash, ':'), and a file that is the same file as a listed one under
//    another spelling (Unicode form, case) is not listed a second time.
//
// Index format, UTF-8 text, one line per sample, fields separated by a tab:
//
//   phylotone.samples.index.v1
//   <file name>\t<slot, or - when unassigned>\t<root note>\t<name>
//
// Not thread-safe. Use it from one thread.
#pragma once

#include "phylo/samples/SampleBlob.h"

#include <cstdint>
#include <string>
#include <vector>

namespace phylo::samples
{

constexpr char kIndexFileName[] = "index.txt";
constexpr char kIndexHeader[] = "phylotone.samples.index.v1";

struct LibraryEntry
{
    std::string file;                         // file name inside the folder, for example "kick.wav"
    std::string name;                         // display name, sent in the blob
    int slot = kNoSlot;                       // 0 to 127, or kNoSlot
    std::uint8_t rootNote = kDefaultRootNote; // 0 to 127
    int channels = 0;                         // 1 or 2; 0 when the file is missing
    std::uint32_t frames = 0;                 // at 48 kHz; 0 when the file is missing
    bool missing = false;                     // listed in the index, not in the folder
};

// `message` is the reason for the status line. Empty when ok.
struct LibraryResult
{
    bool ok = true;
    std::string message;

    static LibraryResult failure(std::string why) { return {false, std::move(why)}; }
};

class SampleLibrary
{
public:
    // `folder` is a UTF-8 path. Nothing is touched until open().
    explicit SampleLibrary(std::string folder);

    // Creates the folder if needed, reads the index, and reconciles it with the files that
    // are there (see the rules above). Safe to call again to pick up changes on disk.
    LibraryResult open();

    const std::string& folder() const noexcept { return folder_; }
    const std::vector<LibraryEntry>& entries() const noexcept { return entries_; }

    // Problems found by the last open() that did not stop it: an unreadable index line, a
    // file in the folder that is not a usable sample, a duplicate slot in the index.
    const std::vector<std::string>& warnings() const noexcept { return warnings_; }

    // Adds a sample that is already 48 kHz float (see SampleImport.h). Writes a new file
    // named after `name`, made unique in the folder, adds an unassigned entry, and saves the
    // index. `fileOut` receives the new file name. Refuses a sample that could not be sent:
    // not mono or stereo, no frames, longer than 60 s, or not finite.
    LibraryResult add(const std::string& name, int channels, const std::vector<float>& samples,
                      std::string& fileOut);

    // Puts the sample in `slot`. A sample already in that slot becomes unassigned (it stays
    // in the library). A sample that was in another slot moves. Refuses an unknown file, a
    // missing file, or a slot outside 0 to 127. Saves the index.
    LibraryResult assign(const std::string& file, int slot);

    // Empties the slot. The sample stays in the library. Clearing an empty slot is ok.
    LibraryResult clearSlot(int slot);

    LibraryResult setRootNote(const std::string& file, int rootNote); // 0 to 127
    LibraryResult rename(const std::string& file, const std::string& name); // display name only

    const LibraryEntry* find(const std::string& file) const noexcept;
    const LibraryEntry* inSlot(int slot) const noexcept;
    int firstFreeSlot() const noexcept; // kNoSlot when all 128 are taken

    // Loads the audio of every assigned, present sample, in ascending slot order, ready for
    // encodeSampleBlob. Fails, leaving `out` empty, if an assigned file cannot be read or is
    // no longer a usable sample; the message names the file.
    LibraryResult buildSlotSet(SampleSlotSet& out) const;

private:
    LibraryResult saveIndex();
    static std::string newerIndexMessage();

    std::string folder_;
    std::vector<LibraryEntry> entries_;
    std::vector<std::string> warnings_;
    bool indexDamaged_ = false; // the index on disk had lines that could not be read
    bool newerIndex_ = false;   // the index on disk is from a newer version; never replace it
};

} // namespace phylo::samples
