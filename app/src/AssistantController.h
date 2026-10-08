// Owns the assistant core for the app: project state, action log, assistant,
// provider and session. Bridges them to the scene file and the UI.
//
// Threading: every public method runs on the message thread. A chat turn
// (Session::send, which may run several tool calls) runs on a single background
// pool thread. While a turn is in flight the background thread owns the
// project, log, assistant and session; the message thread does not touch them
// (approve, reject, settings changes and file refreshes wait until the turn
// ends). The reply comes back through MessageManager::callAsync. Only one turn
// is in flight at a time.
//
// Scene file: the project's scene text is synced from the file before every
// turn and approval, and written back after any applied change, so the window's
// existing reload path picks it up (on the next bar while playing). If the file
// changed on disk during a turn, the assistant's result is not written over it.

#pragma once

#include "AssistantSettings.h"
#include "JuceHttpPost.h"

#include "phylo/assistant/Assistant.h"

#include <juce_events/juce_events.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class AssistantController
{
public:
    struct ProposalInfo
    {
        int id = 0;
        juce::String summary; // tool name and reason, for display
    };

    AssistantController(juce::File sceneFile, juce::File settingsFile);
    ~AssistantController();

    const AssistantSettings& settings() const noexcept { return settings_; }

    // Settings changes are saved at once. Changing provider, model or base URL
    // starts a new conversation. Ignored while a turn is in flight.
    void setProvider(ProviderKind p);
    void setModel(const juce::String& model);
    void setLocalBaseUrl(const juce::String& url);
    void setMode(phylo::assistant::Mode m);

    bool isBusy() const noexcept { return busy; }

    // Starts a turn. Returns false (and reports why) if it could not start.
    bool send(const juce::String& text);

    // Snapshot of pending proposals, refreshed when no turn is in flight.
    const std::vector<ProposalInfo>& proposals() const noexcept { return proposalSnapshot; }

    void approve(int proposalId);
    void reject(int proposalId);

    // Call periodically (the window's scene timer). Refreshes the scene text
    // when the file changed on disk.
    void pollSceneFile();

    // UI hooks, called on the message thread.
    std::function<void(const juce::String& who, const juce::String& text)> onTranscript;
    std::function<void()> onStateChanged;

    static constexpr int kHttpTimeoutMs = 60000;

private:
    void say(const juce::String& who, const juce::String& text);
    void notifyChanged();

    void saveSettings();
    void resetSession(const juce::String& why);
    bool ensureSession();

    // Reads the file into project.sceneText. An empty file (save in progress)
    // keeps the current text. Returns false if the file could not be read.
    bool syncFromFile();
    juce::String readSceneFile() const;

    // Writes project.sceneText if it differs from `textBefore`, provided the
    // file still holds `textBefore`. Otherwise reloads from the file.
    void writeIfChanged(const std::string& textBefore);

    void finishTurn(const phylo::assistant::TurnResult& result, std::string textBefore);
    void refreshProposals();

    juce::File sceneFile;
    juce::File settingsFile;
    juce::Time sceneModified;

    AssistantSettings settings_;

    phylo::assistant::ProjectState project;
    phylo::assistant::ActionLog log;
    phylo::assistant::Assistant assistant { project, log };

    std::shared_ptr<HttpCancel> cancel = std::make_shared<HttpCancel>();
    std::unique_ptr<phylo::assistant::ChatProvider> provider;
    std::unique_ptr<phylo::assistant::Session> session;

    std::vector<ProposalInfo> proposalSnapshot;
    bool busy = false;

    // Set false in the destructor so late callAsync replies are dropped.
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);

    juce::ThreadPool pool { juce::ThreadPoolOptions{}.withThreadName("Assistant").withNumberOfThreads(1) };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AssistantController)
};
