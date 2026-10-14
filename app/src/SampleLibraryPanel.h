#pragma once

#include "SampleSession.h"

#include <functional>
#include <memory>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

// Lists the sample library and lets the user import a WAV, put a sample in a slot, clear a
// slot and set the root note. Message thread only. Nothing here deletes a file.
class SampleLibraryPanel : public juce::Component, private juce::ListBoxModel
{
public:
    // The session must outlive the panel.
    explicit SampleLibraryPanel(SampleSession& session);
    ~SampleLibraryPanel() override;

    // Fired after an assign, a clear or a root note change that really changed a slot.
    std::function<void()> onSlotsChanged;
    // The latest result for the status line, for example "Imported kick.wav".
    std::function<void(const juce::String&)> onMessage;

    // Imports each .wav path, refreshes the list, selects the last one imported and
    // reports through onMessage. Other paths are refused.
    void importFiles(const juce::StringArray& paths);

    // Re-reads the library and repaints. Keeps the selection by file name when it can.
    void refresh();

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    // ListBoxModel
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override;
    void selectedRowsChanged(int lastRowSelected) override;

    const phylo::samples::LibraryEntry* selectedEntry() const;
    void updateControls();
    void chooseFiles();
    void assignSelected();
    void clearSelected();
    void rootNoteChosen();
    void report(const juce::String& text);

    SampleSession& session;
    std::vector<phylo::samples::LibraryEntry> rows;

    juce::ListBox list;
    juce::TextButton importButton { "Import WAV..." };
    juce::ComboBox slotBox;
    juce::TextButton assignButton { "Assign" };
    juce::TextButton clearButton { "Clear slot" };
    juce::Label rootLabel;
    juce::ComboBox rootBox;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SampleLibraryPanel)
};
