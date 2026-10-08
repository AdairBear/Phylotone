// In-window assistant panel: provider settings, transcript, input and the
// pending proposals with Approve and Reject. All colours come from the
// LookAndFeel; the panel paints nothing itself, so the window background shows.

#pragma once

#include "AssistantController.h"

#include <juce_gui_basics/juce_gui_basics.h>

class AssistantPanel : public juce::Component
{
public:
    explicit AssistantPanel(AssistantController& controller);
    ~AssistantPanel() override;

    void resized() override;

private:
    class ProposalRow;
    class ProposalList : public juce::Component
    {
    public:
        void resized() override;
        juce::OwnedArray<ProposalRow> rows;
    };

    void appendTranscript(const juce::String& who, const juce::String& text);
    void syncFromController();
    void rebuildProposals();
    void sendInput();
    void commitModel();
    juce::String modelHint() const;

    AssistantController& controller;

    juce::Label titleLabel;
    juce::ComboBox providerBox;
    juce::ComboBox modeBox;
    juce::Label modelLabel;
    juce::TextEditor modelEditor;
    juce::Label urlLabel;
    juce::TextEditor urlEditor;
    juce::Label hintLabel;
    juce::TextEditor transcript;
    juce::Label proposalsLabel;
    juce::Viewport proposalsView;
    ProposalList proposalList;
    juce::TextEditor input;
    juce::TextButton sendButton { "Send" };
    juce::Label statusLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AssistantPanel)
};
