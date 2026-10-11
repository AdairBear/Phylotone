#include "SampleLibraryPanel.h"

#include <string>

namespace
{
constexpr int kGapPx = 8;
constexpr int kControlRowPx = 28;
constexpr int kRowPx = 22;
constexpr double kFrameRateHz = 48000.0; // the library stores 48 kHz

// MIDI note 60 is C4.
juce::String noteText(int note)
{
    static const char* const names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return juce::String(note) + " (" + names[note % 12] + juce::String(note / 12 - 1) + ")";
}

bool isWavPath(const juce::String& path)
{
    return path.endsWithIgnoreCase(".wav");
}
} // namespace

SampleLibraryPanel::SampleLibraryPanel(SampleSession& s) : session(s)
{
    list.setModel(this);
    list.setRowHeight(kRowPx);
    list.setColour(juce::ListBox::backgroundColourId, juce::Colour(0xff23262b));
    list.setColour(juce::ListBox::outlineColourId, juce::Colour(0xff3a3d44));
    list.setOutlineThickness(1);

    for (int slot = 0; slot < phylo::samples::kSlotCount; ++slot)
        slotBox.addItem("slot " + juce::String(slot), slot + 1);
    for (int note = 0; note <= 127; ++note)
        rootBox.addItem(noteText(note), note + 1);
    rootBox.setSelectedId(phylo::samples::kDefaultRootNote + 1, juce::dontSendNotification);

    rootLabel.setText("Root", juce::dontSendNotification);
    rootLabel.setJustificationType(juce::Justification::centredRight);

    importButton.onClick = [this] { chooseFiles(); };
    assignButton.onClick = [this] { assignSelected(); };
    clearButton.onClick = [this] { clearSelected(); };
    rootBox.onChange = [this] { rootNoteChosen(); };

    addAndMakeVisible(list);
    addAndMakeVisible(importButton);
    addAndMakeVisible(slotBox);
    addAndMakeVisible(assignButton);
    addAndMakeVisible(clearButton);
    addAndMakeVisible(rootLabel);
    addAndMakeVisible(rootBox);

    refresh();
}

SampleLibraryPanel::~SampleLibraryPanel()
{
    list.setModel(nullptr);
}

void SampleLibraryPanel::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff1b1d21));
}

void SampleLibraryPanel::resized()
{
    auto area = getLocalBounds();
    auto controls = area.removeFromBottom(kControlRowPx);
    area.removeFromBottom(kGapPx);
    list.setBounds(area);

    importButton.setBounds(controls.removeFromLeft(112));
    controls.removeFromLeft(kGapPx);
    slotBox.setBounds(controls.removeFromLeft(88));
    controls.removeFromLeft(kGapPx);
    assignButton.setBounds(controls.removeFromLeft(68));
    controls.removeFromLeft(kGapPx);
    clearButton.setBounds(controls.removeFromLeft(88));
    controls.removeFromLeft(kGapPx);
    rootLabel.setBounds(controls.removeFromLeft(40));
    controls.removeFromLeft(4);
    rootBox.setBounds(controls.removeFromLeft(104));
}

int SampleLibraryPanel::getNumRows()
{
    return static_cast<int>(rows.size());
}

void SampleLibraryPanel::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected)
{
    if (row < 0 || row >= static_cast<int>(rows.size()))
        return;
    const auto& e = rows[static_cast<size_t>(row)];

    if (selected)
        g.fillAll(juce::Colour(0xff35507a));

    const auto text = e.missing ? juce::Colour(0xffe0a060) : juce::Colour(0xffd8dadf);
    g.setColour(text);
    g.setFont(14.0f);

    const juce::String slot = e.slot == phylo::samples::kNoSlot ? juce::String(juce::CharPointer_UTF8("\xe2\x80\x94"))
                                                                : "slot " + juce::String(e.slot);
    g.drawText(slot, 8, 0, 60, height, juce::Justification::centredLeft, true);
    g.drawText(juce::String::fromUTF8(e.name.c_str()), 72, 0, width - 72 - 300, height,
               juce::Justification::centredLeft, true);

    const int right = width - 300;
    if (e.missing)
    {
        g.drawText("missing", right, 0, 292, height, juce::Justification::centredRight, true);
        return;
    }
    const double seconds = static_cast<double>(e.frames) / kFrameRateHz;
    g.drawText(e.channels == 1 ? "mono" : "stereo", right, 0, 70, height, juce::Justification::centredLeft, true);
    g.drawText(juce::String(seconds, 2) + " s", right + 70, 0, 80, height, juce::Justification::centredLeft, true);
    g.drawText("root " + noteText(e.rootNote), right + 150, 0, 142, height, juce::Justification::centredRight, true);
}

void SampleLibraryPanel::selectedRowsChanged(int)
{
    updateControls();
}

const phylo::samples::LibraryEntry* SampleLibraryPanel::selectedEntry() const
{
    const int row = list.getSelectedRow();
    if (row < 0 || row >= static_cast<int>(rows.size()))
        return nullptr;
    return &rows[static_cast<size_t>(row)];
}

void SampleLibraryPanel::updateControls()
{
    const auto* e = selectedEntry();
    const bool usable = e != nullptr && !e->missing;

    assignButton.setEnabled(usable);
    clearButton.setEnabled(e != nullptr && e->slot != phylo::samples::kNoSlot);
    slotBox.setEnabled(usable);
    rootBox.setEnabled(usable);
    if (e == nullptr)
        return;

    rootBox.setSelectedId(e->rootNote + 1, juce::dontSendNotification);

    // Offer the slot the sample is in, or else the first free one.
    int slot = e->slot;
    if (slot == phylo::samples::kNoSlot)
        slot = session.library().firstFreeSlot();
    if (slot == phylo::samples::kNoSlot)
        slot = 0;
    slotBox.setSelectedId(slot + 1, juce::dontSendNotification);
}

void SampleLibraryPanel::refresh()
{
    juce::String keep;
    if (const auto* e = selectedEntry())
        keep = juce::String::fromUTF8(e->file.c_str());

    rows = session.library().entries();
    list.updateContent();

    int row = -1;
    for (size_t i = 0; i < rows.size(); ++i)
        if (juce::String::fromUTF8(rows[i].file.c_str()) == keep)
            row = static_cast<int>(i);
    if (row >= 0)
        list.selectRow(row, false, true);
    else
        list.deselectAllRows();

    list.repaint();
    updateControls();
}

void SampleLibraryPanel::report(const juce::String& text)
{
    if (onMessage)
        onMessage(text);
}

void SampleLibraryPanel::importFiles(const juce::StringArray& paths)
{
    int imported = 0;
    juce::String firstRefusal;
    juce::String lastFile;
    juce::String lastShown;

    for (const auto& path : paths)
    {
        const auto shown = juce::File(path).getFileName();
        if (!isWavPath(path))
        {
            if (firstRefusal.isEmpty())
                firstRefusal = shown + ": not a WAV file";
            continue;
        }

        std::string file;
        const auto result = session.importFile(path.toStdString(), file);
        if (result.ok)
        {
            ++imported;
            lastFile = juce::String::fromUTF8(file.c_str());
            lastShown = shown;
        }
        else if (firstRefusal.isEmpty())
        {
            firstRefusal = juce::String::fromUTF8(result.message.c_str());
        }
    }

    refresh();
    if (lastFile.isNotEmpty())
    {
        for (size_t i = 0; i < rows.size(); ++i)
            if (juce::String::fromUTF8(rows[i].file.c_str()) == lastFile)
                list.selectRow(static_cast<int>(i));
    }

    if (paths.size() == 1)
        report(imported == 1 ? "Imported " + lastShown : "Not imported: " + firstRefusal);
    else if (imported == 0)
        report("Imported 0 of " + juce::String(paths.size()) + " files. " + firstRefusal);
    else if (firstRefusal.isEmpty())
        report("Imported " + juce::String(imported) + " files");
    else
        report("Imported " + juce::String(imported) + " of " + juce::String(paths.size()) +
               " files. First refusal: " + firstRefusal);
}

void SampleLibraryPanel::chooseFiles()
{
    chooser = std::make_unique<juce::FileChooser>("Import WAV files", juce::File(), "*.wav");
    const auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles |
                       juce::FileBrowserComponent::canSelectMultipleItems;

    juce::Component::SafePointer<SampleLibraryPanel> self(this);
    chooser->launchAsync(flags,
                         [self](const juce::FileChooser& fc)
                         {
                             if (self == nullptr)
                                 return;
                             juce::StringArray paths;
                             for (const auto& f : fc.getResults())
                                 paths.add(f.getFullPathName());
                             if (!paths.isEmpty())
                                 self->importFiles(paths);
                         });
}

void SampleLibraryPanel::assignSelected()
{
    const auto* e = selectedEntry();
    if (e == nullptr || e->missing || slotBox.getSelectedId() <= 0)
        return;

    const std::string file = e->file;
    const int slot = slotBox.getSelectedId() - 1;
    const bool same = e->slot == slot;
    const auto name = juce::String::fromUTF8(e->name.c_str());

    const auto result = session.assign(file, slot);
    refresh();
    if (!result.ok)
    {
        report(juce::String::fromUTF8(result.message.c_str()));
        return;
    }
    report(same ? name + " is already in slot " + juce::String(slot) : name + " is in slot " + juce::String(slot));
    if (!same && onSlotsChanged)
        onSlotsChanged();
}

void SampleLibraryPanel::clearSelected()
{
    const auto* e = selectedEntry();
    if (e == nullptr || e->slot == phylo::samples::kNoSlot)
        return;

    const int slot = e->slot;
    const auto result = session.clearSlot(slot);
    refresh();
    if (!result.ok)
    {
        report(juce::String::fromUTF8(result.message.c_str()));
        return;
    }
    report("Cleared slot " + juce::String(slot));
    if (onSlotsChanged)
        onSlotsChanged();
}

void SampleLibraryPanel::rootNoteChosen()
{
    const auto* e = selectedEntry();
    if (e == nullptr || e->missing || rootBox.getSelectedId() <= 0)
        return;

    const int note = rootBox.getSelectedId() - 1;
    if (note == e->rootNote)
        return;

    const bool inSlot = e->slot != phylo::samples::kNoSlot;
    const auto result = session.setRootNote(e->file, note);
    refresh();
    if (!result.ok)
    {
        report(juce::String::fromUTF8(result.message.c_str()));
        return;
    }
    report("Root note " + noteText(note));
    // The blob only carries the root note of a sample that is in a slot.
    if (inSlot && onSlotsChanged)
        onSlotsChanged();
}
