// Assistant core (M2): tools, modes, action log with undo, and the turn loop.
//
// The assistant changes a project only through the named tools below. Every
// change is planned first, then either applied (with a log entry that records
// what changed, why, and the previous text) or held as a proposal for the
// musician to approve.
//
// Modes (per project):
//   Off    read-only tools only
//   Ask    every change becomes a proposal; nothing is applied without approval
//   Assist low-risk changes apply; destructive changes become proposals

#pragma once

#include "phylo/assistant/ChatProvider.h"
#include "phylo/assistant/Json.h"

#include <functional>
#include <string>
#include <vector>

namespace phylo::assistant
{

enum class Mode { Off, Ask, Assist };

const char* modeName(Mode m) noexcept;
bool parseMode(const std::string& s, Mode& out) noexcept;

// The project as the assistant sees it. The scene text is the source of truth.
struct ProjectState
{
    std::string sceneText;
    Mode mode = Mode::Ask;
};

struct LogEntry
{
    int seq = 0;
    std::string tool;
    std::string argsJson;
    std::string reason;
    std::string beforeText; // scene text before the change, for undo
    std::string afterText;
};

// Ordered log of applied changes. Undo pops the last entry and restores its text.
class ActionLog
{
public:
    void append(LogEntry e);
    const std::vector<LogEntry>& entries() const noexcept { return entries_; }
    bool popLast(LogEntry& out);

    // One JSON object per line. Strings are quoted, so every line stays valid.
    std::string toJsonLines() const;

private:
    std::vector<LogEntry> entries_;
    int nextSeq_ = 1;
};

struct Proposal
{
    int id = 0;
    ToolCall call;
    std::string baseText; // scene text when proposed; approval is refused if it changed
};

struct ToolResult
{
    bool ok = false;        // the call was understood and did what it said
    bool applied = false;   // a change was written
    bool proposed = false;  // a change is waiting for approval
    std::string message;    // short, human-readable; sent back to the model
    std::string resultJson; // read tools: the result object as JSON text
    int proposalId = 0;
};

class Assistant
{
public:
    Assistant(ProjectState& project, ActionLog& log);

    std::vector<ToolSpec> tools() const;
    std::string systemPrompt() const;

    // Runs one tool call under the current mode.
    ToolResult invoke(const ToolCall& call);

    // Called after each change to the scene (an applied edit or an undo), with the new
    // scene text. Runs on whichever thread made the change.
    std::function<void(const std::string& sceneText)> onSceneChanged;

    const std::vector<Proposal>& proposals() const noexcept { return proposals_; }

    // Explicit user actions only. Approve re-plans against the current project,
    // so a proposal made against older text cannot overwrite newer edits.
    ToolResult approve(int proposalId);
    bool reject(int proposalId);

private:
    struct Plan
    {
        bool ok = false;
        std::string error;
        std::string tool;
        std::string argsJson;
        std::string reason;
        std::string afterText;
        bool destructive = false;
    };

    Plan plan(const ToolCall& call) const;
    Plan planEditScene(const json::Value& args, const std::string& reason) const;
    Plan planSetMacro(const json::Value& args, const std::string& reason) const;
    Plan planUndo(const std::string& reason) const;
    void apply(const Plan& p);

    ProjectState& project;
    ActionLog& log;
    std::vector<Proposal> proposals_;
    int nextProposalId = 1;
};

struct TurnResult
{
    bool ok = false;
    std::string error;
    std::string text;   // the assistant's final reply
    int toolCalls = 0;  // tool calls run in this turn
};

// A conversation with one provider. Holds the message history between turns.
class Session
{
public:
    Session(Assistant& assistant, ChatProvider& provider, std::string model);

    // Sends one user message and runs tool calls until the model answers in text.
    TurnResult send(const std::string& userText);

    // Records something the app did that the model should know about (for example,
    // the user approved or rejected a proposal). It is sent ahead of the next user
    // message, marked as not coming from the user.
    void noteAppAction(const std::string& note);

    const std::vector<Message>& history() const noexcept { return history_; }
    void clear() { history_.clear(); }

    static constexpr int kMaxToolRounds = 6;

private:
    Assistant& assistant;
    ChatProvider& provider;
    std::string model;
    std::vector<Message> history_;
    std::vector<std::string> pendingNotes_;
};

} // namespace phylo::assistant
