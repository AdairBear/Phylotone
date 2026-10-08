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
bool buildBody(const ChatRequest& req, bool useMaxCompletionTokens, json& body,
               std::string& error)
{
    body = json::object();
    body["model"] = req.model;
    body[useMaxCompletionTokens ? "max_completion_tokens" : "max_tokens"] =
        req.maxOutputTokens;

    json messages = json::array();
    if (!req.systemPrompt.empty())
        messages.push_back({{"role", "system"}, {"content", req.systemPrompt}});

    for (const auto& m : req.messages)
    {
        switch (m.role)
        {
        case Role::User:
            messages.push_back({{"role", "user"}, {"content", m.text}});
            break;
        case Role::Tool:
            messages.push_back(
                {{"role", "tool"}, {"tool_call_id", m.toolCallId}, {"content", m.text}});
            break;
        case Role::Assistant:
        {
            json msg = {{"role", "assistant"}};
            if (m.text.empty() && !m.toolCalls.empty())
                msg["content"] = nullptr;
            else
                msg["content"] = m.text;
            if (!m.toolCalls.empty())
            {
                json calls = json::array();
                for (const auto& tc : m.toolCalls)
                {
                    calls.push_back({{"id", tc.id},
                                     {"type", "function"},
                                     {"function",
                                      {{"name", tc.name},
                                       {"arguments", tc.argsJson.empty() ? "{}" : tc.argsJson}}}});
                }
                msg["tool_calls"] = std::move(calls);
            }
            messages.push_back(std::move(msg));
            break;
        }
        }
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
            tools.push_back({{"type", "function"},
                             {"function",
                              {{"name", t.name},
                               {"description", t.description},
                               {"parameters", schema}}}});
        }
        body["tools"] = std::move(tools);
    }
    return true;
}

ChatResponse parseResponse(const std::string& text, const std::string& vendor)
{
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return fail(vendor + ": response is not valid JSON");
    if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty()
        || !j["choices"][0].is_object() || !j["choices"][0].contains("message")
        || !j["choices"][0]["message"].is_object())
        return fail(vendor + ": response has no choices[0].message");

    const json& msg = j["choices"][0]["message"];
    ChatResponse r;
    if (msg.contains("content") && msg["content"].is_string())
        r.text = msg["content"].get<std::string>();

    if (msg.contains("tool_calls") && msg["tool_calls"].is_array())
    {
        int index = 0;
        for (const auto& call : msg["tool_calls"])
        {
            if (!call.is_object() || !call.contains("function") || !call["function"].is_object())
            {
                ++index;
                continue;
            }
            const json& fn = call["function"];
            ToolCall tc;
            if (call.contains("id") && call["id"].is_string() && !call["id"].get<std::string>().empty())
                tc.id = call["id"].get<std::string>();
            else
                tc.id = vendor + "-" + std::to_string(index);
            if (fn.contains("name") && fn["name"].is_string())
                tc.name = fn["name"].get<std::string>();
            if (fn.contains("arguments") && fn["arguments"].is_string())
                tc.argsJson = fn["arguments"].get<std::string>();
            else if (fn.contains("arguments") && fn["arguments"].is_object())
                tc.argsJson = dumpJson(fn["arguments"]); // some local servers send an object
            else
                tc.argsJson = "{}";
            r.toolCalls.push_back(std::move(tc));
            ++index;
        }
    }
    r.ok = true;
    return r;
}
} // namespace

namespace detail
{
ChatResponse openAIComplete(const HttpPost& post,
                            const std::string& baseUrl,
                            const std::string& key,
                            const std::string& vendor,
                            bool useMaxCompletionTokens,
                            const ChatRequest& request)
{
    try
    {
        json body;
        std::string error;
        if (!buildBody(request, useMaxCompletionTokens, body, error))
            return fail(vendor + ": " + error, key);

        HttpHeaders headers = {{"content-type", "application/json"}};
        if (!key.empty())
            headers.emplace_back("authorization", "Bearer " + key);

        std::string responseBody;
        ChatResponse failure;
        if (!send(post, joinUrl(baseUrl, "chat/completions"), headers, dumpJson(body), vendor,
                  key, responseBody, failure))
            return failure;
        return parseResponse(responseBody, vendor);
    }
    catch (...)
    {
        return fail(vendor + ": unexpected error", key);
    }
}
} // namespace detail

OpenAIProvider::OpenAIProvider(HttpPost post, std::string baseUrl)
    : post_(std::move(post)), baseUrl_(std::move(baseUrl))
{
}

ChatResponse OpenAIProvider::complete(const ChatRequest& request)
{
    try
    {
        const std::string key = envValue("OPENAI_API_KEY");
        if (key.empty())
            return fail("openai: OPENAI_API_KEY is not set");
        return openAIComplete(post_, baseUrl_, key, "openai", true, request);
    }
    catch (...)
    {
        return fail("openai: unexpected error");
    }
}

OpenAICompatibleProvider::OpenAICompatibleProvider(HttpPost post,
                                                   std::string baseUrl,
                                                   std::string keyEnvName,
                                                   std::string displayName)
    : post_(std::move(post)),
      baseUrl_(std::move(baseUrl)),
      keyEnvName_(std::move(keyEnvName)),
      displayName_(std::move(displayName))
{
}

ChatResponse OpenAICompatibleProvider::complete(const ChatRequest& request)
{
    try
    {
        // Key is optional; absent or empty means no Authorization header.
        const std::string key = envValue(keyEnvName_);
        return openAIComplete(post_, baseUrl_, key, displayName_, false, request);
    }
    catch (...)
    {
        return fail(displayName_ + ": unexpected error");
    }
}

} // namespace phylo::providers
