#include "Common.h"

namespace phylo::providers
{

using namespace detail;
using assistant::ChatRequest;
using assistant::ChatResponse;
using assistant::Role;
using assistant::ToolCall;

namespace
{
const char* kVendor = "anthropic";
const char* kKeyEnv = "ANTHROPIC_API_KEY";

// Builds the request body. Returns false with error set on bad input.
bool buildBody(const ChatRequest& req, json& body, std::string& error)
{
    body = json::object();
    body["model"] = req.model;
    body["max_tokens"] = req.maxOutputTokens;
    if (!req.systemPrompt.empty())
        body["system"] = req.systemPrompt;

    json messages = json::array();
    for (const auto& m : req.messages)
    {
        if (m.role == Role::Tool)
        {
            json block = {{"type", "tool_result"},
                          {"tool_use_id", m.toolCallId},
                          {"content", m.text}};
            // Consecutive tool results share one user message, as the API expects
            // all results for one assistant turn in the next user turn.
            if (!messages.empty() && messages.back()["role"] == "user"
                && !messages.back()["content"].empty()
                && messages.back()["content"].back()["type"] == "tool_result")
            {
                messages.back()["content"].push_back(std::move(block));
            }
            else
            {
                messages.push_back({{"role", "user"}, {"content", json::array({block})}});
            }
            continue;
        }

        json content = json::array();
        if (!m.text.empty() || m.toolCalls.empty())
            content.push_back({{"type", "text"}, {"text", m.text}});
        for (const auto& tc : m.toolCalls)
        {
            json input;
            if (!parseObject(tc.argsJson, input, true))
            {
                error = "tool call '" + tc.name + "' has arguments that are not a JSON object";
                return false;
            }
            content.push_back(
                {{"type", "tool_use"}, {"id", tc.id}, {"name", tc.name}, {"input", input}});
        }
        messages.push_back(
            {{"role", m.role == Role::Assistant ? "assistant" : "user"}, {"content", content}});
    }
    body["messages"] = std::move(messages);

    if (!req.tools.empty())
    {
        json tools = json::array();
        for (const auto& t : req.tools)
        {
            json schema;
            if (!parseObject(t.parametersJsonSchema, schema, true))
            {
                error = "tool '" + t.name + "' has a schema that is not a JSON object";
                return false;
            }
            if (schema.empty())
                schema = {{"type", "object"}, {"properties", json::object()}};
            tools.push_back(
                {{"name", t.name}, {"description", t.description}, {"input_schema", schema}});
        }
        body["tools"] = std::move(tools);
    }
    return true;
}

ChatResponse parseResponse(const std::string& text)
{
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return fail("anthropic: response is not valid JSON");
    if (!j.contains("content") || !j["content"].is_array())
        return fail("anthropic: response has no content array");

    ChatResponse r;
    int index = 0;
    for (const auto& block : j["content"])
    {
        if (!block.is_object() || !block.contains("type") || !block["type"].is_string())
            continue;
        const auto type = block["type"].get<std::string>();
        if (type == "text" && block.contains("text") && block["text"].is_string())
        {
            r.text += block["text"].get<std::string>();
        }
        else if (type == "tool_use")
        {
            ToolCall tc;
            if (block.contains("id") && block["id"].is_string())
                tc.id = block["id"].get<std::string>();
            else
                tc.id = "anthropic-" + std::to_string(index);
            if (block.contains("name") && block["name"].is_string())
                tc.name = block["name"].get<std::string>();
            tc.argsJson = block.contains("input") ? dumpJson(block["input"]) : "{}";
            r.toolCalls.push_back(std::move(tc));
        }
        // Other block types (e.g. thinking) are ignored.
        ++index;
    }
    r.ok = true;
    return r;
}
} // namespace

AnthropicProvider::AnthropicProvider(HttpPost post, std::string baseUrl)
    : post_(std::move(post)), baseUrl_(std::move(baseUrl))
{
}

ChatResponse AnthropicProvider::complete(const ChatRequest& request)
{
    std::string key;
    try
    {
        key = envValue(kKeyEnv);
        if (key.empty())
            return fail(std::string(kVendor) + ": " + kKeyEnv + " is not set");

        json body;
        std::string error;
        if (!buildBody(request, body, error))
            return fail(std::string(kVendor) + ": " + error, key);

        const HttpHeaders headers = {{"x-api-key", key},
                                     {"anthropic-version", "2023-06-01"},
                                     {"content-type", "application/json"}};
        std::string responseBody;
        ChatResponse failure;
        if (!send(post_, joinUrl(baseUrl_, "messages"), headers, dumpJson(body), kVendor, key,
                  responseBody, failure))
            return failure;
        return parseResponse(responseBody);
    }
    catch (...)
    {
        return fail(std::string(kVendor) + ": unexpected error", key);
    }
}

} // namespace phylo::providers
