// Provider-neutral chat interface for the assistant (M2).
//
// The assistant core talks only to this interface. Each model vendor, and any
// OpenAI-compatible local server, is an adapter that implements ChatProvider.
// Adapters live in the app layer and do the HTTP. This header has no dependencies
// beyond the standard library, so the core stays JUCE-free and testable.
//
// Contract for adapters:
//  - complete() must not throw. Failures come back as ChatResponse::ok == false
//    with a short, human-readable error. Never include the API key in an error.
//  - Tool arguments are passed as JSON text, exactly as the model produced them.
//  - A tool result is sent back as a Message with role Tool, referencing the
//    ToolCall::id it answers. Adapters map this to their vendor's format.
//  - Keys are read from the environment by the adapter, never from project files.

#pragma once

#include <string>
#include <vector>

namespace phylo::assistant
{

enum class Role { User, Assistant, Tool };

struct ToolCall
{
    std::string id;       // vendor id; adapters synthesise one if the vendor gives none
    std::string name;     // tool name, e.g. "edit_scene"
    std::string argsJson; // arguments as a JSON object, as text
};

struct Message
{
    Role role = Role::User;
    std::string text;                 // user or assistant text; tool result text for Tool
    std::vector<ToolCall> toolCalls;  // set on Assistant messages that request tools
    std::string toolCallId;           // set on Tool messages: the ToolCall::id answered
};

struct ToolSpec
{
    std::string name;
    std::string description;
    std::string parametersJsonSchema; // a JSON Schema object, as text
};

struct ChatRequest
{
    std::string systemPrompt;
    std::vector<Message> messages;
    std::vector<ToolSpec> tools;
    std::string model;      // model id, as the vendor names it; chosen by the user
    int maxOutputTokens = 1024;
};

struct ChatResponse
{
    bool ok = false;
    std::string error;                // set when !ok
    std::string text;                 // assistant text, may be empty when only tools are called
    std::vector<ToolCall> toolCalls;  // tools the model asked to run
};

class ChatProvider
{
public:
    virtual ~ChatProvider() = default;

    // Short name for display and logs, e.g. "anthropic", "openai", "gemini", "local".
    virtual std::string name() const = 0;

    virtual ChatResponse complete(const ChatRequest& request) = 0;
};

} // namespace phylo::assistant
