#include "Common.h"

#include <map>

namespace phylo::providers
{

using namespace detail;
using assistant::ChatRequest;
using assistant::ChatResponse;
using assistant::Role;
using assistant::ToolCall;

namespace
{
const char* kVendor = "gemini";
const char* kKeyEnv = "GEMINI_API_KEY";

// The model id goes into the URL path, so only allow a safe character set.
bool validModel(const std::string& model)
{
    if (model.empty())
        return false;
    for (char c : model)
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                        || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!ok)
            return false;
    }
    return true;
}

bool buildBody(const ChatRequest& req, json& body, std::string& error)
{
    body = json::object();
    if (!req.systemPrompt.empty())
        body["system_instruction"] = {{"parts", json::array({{{"text", req.systemPrompt}}})}};

    // Gemini has no call ids. Remember which name each (synthesised) id stands for
    // so a Tool message can be mapped back to a functionResponse by name. Later
    // assistant turns overwrite earlier ones, since ids restart at gemini-0.
    std::map<std::string, std::string> nameForId;

    json contents = json::array();
    for (const auto& m : req.messages)
    {
        if (m.role == Role::Tool)
        {
            const auto it = nameForId.find(m.toolCallId);
            if (it == nameForId.end())
            {
                error = "tool result '" + m.toolCallId + "' does not answer a known tool call";
                return false;
            }
            json response;
            if (!parseObject(m.text, response, false))
                response = {{"content", m.text}}; // response must be an object
            json part = {{"functionResponse", {{"name", it->second}, {"response", response}}}};
            // Results for one model turn go together in one user turn.
            if (!contents.empty() && contents.back()["role"] == "user"
                && !contents.back()["parts"].empty()
                && contents.back()["parts"].back().contains("functionResponse"))
            {
                contents.back()["parts"].push_back(std::move(part));
            }
            else
            {
                contents.push_back({{"role", "user"}, {"parts", json::array({part})}});
            }
            continue;
        }

        json parts = json::array();
        if (!m.text.empty() || m.toolCalls.empty())
            parts.push_back({{"text", m.text}});
        for (const auto& tc : m.toolCalls)
        {
            json args;
            if (!parseObject(tc.argsJson, args, true))
            {
                error = "tool call '" + tc.name + "' has arguments that are not a JSON object";
                return false;
            }
            nameForId[tc.id] = tc.name;
            parts.push_back({{"functionCall", {{"name", tc.name}, {"args", args}}}});
        }
        contents.push_back(
            {{"role", m.role == Role::Assistant ? "model" : "user"}, {"parts", parts}});
    }
    body["contents"] = std::move(contents);

    if (!req.tools.empty())
    {
        json decls = json::array();
        for (const auto& t : req.tools)
        {
            json schema;
            if (!parseObject(t.parametersJsonSchema, schema, true))
            {
                error = "tool '" + t.name + "' has a schema that is not a JSON object";
                return false;
            }
            json decl = {{"name", t.name}, {"description", t.description}};
            if (!schema.empty())
                decl["parameters"] = schema;
            decls.push_back(std::move(decl));
        }
        body["tools"] = json::array({{{"functionDeclarations", decls}}});
    }

    body["generationConfig"] = {{"maxOutputTokens", req.maxOutputTokens}};
    return true;
}

ChatResponse parseResponse(const std::string& text)
{
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return fail("gemini: response is not valid JSON");
    if (!j.contains("candidates") || !j["candidates"].is_array() || j["candidates"].empty())
    {
        // A blocked prompt returns no candidates and a promptFeedback.blockReason.
        if (j.contains("promptFeedback") && j["promptFeedback"].is_object()
            && j["promptFeedback"].contains("blockReason")
            && j["promptFeedback"]["blockReason"].is_string())
            return fail("gemini: prompt blocked: "
                        + j["promptFeedback"]["blockReason"].get<std::string>());
        return fail("gemini: response has no candidates");
    }
    const json& cand = j["candidates"][0];
    ChatResponse r;
    if (!cand.is_object() || !cand.contains("content") || !cand["content"].is_object()
        || !cand["content"].contains("parts") || !cand["content"]["parts"].is_array())
    {
        if (cand.is_object() && cand.contains("finishReason") && cand["finishReason"].is_string())
            return fail("gemini: no content, finishReason "
                        + cand["finishReason"].get<std::string>());
        return fail("gemini: response has no candidates[0].content.parts");
    }

    int callIndex = 0;
    for (const auto& part : cand["content"]["parts"])
    {
        if (!part.is_object())
            continue;
        if (part.contains("thought") && part["thought"].is_boolean() && part["thought"].get<bool>())
            continue; // thought summaries are not answer text
        if (part.contains("text") && part["text"].is_string())
        {
            r.text += part["text"].get<std::string>();
        }
        else if (part.contains("functionCall") && part["functionCall"].is_object())
        {
            const json& fc = part["functionCall"];
            ToolCall tc;
            tc.id = "gemini-" + std::to_string(callIndex++);
            if (fc.contains("name") && fc["name"].is_string())
                tc.name = fc["name"].get<std::string>();
            tc.argsJson = fc.contains("args") ? dumpJson(fc["args"]) : "{}";
            r.toolCalls.push_back(std::move(tc));
        }
    }
    r.ok = true;
    return r;
}
} // namespace

GeminiProvider::GeminiProvider(HttpPost post, std::string baseUrl)
    : post_(std::move(post)), baseUrl_(std::move(baseUrl))
{
}

ChatResponse GeminiProvider::complete(const ChatRequest& request)
{
    std::string key;
    try
    {
        key = envValue(kKeyEnv);
        if (key.empty())
            return fail(std::string(kVendor) + ": " + kKeyEnv + " is not set");

        std::string model = request.model;
        if (model.rfind("models/", 0) == 0)
            model = model.substr(7);
        if (!validModel(model))
            return fail(std::string(kVendor) + ": invalid model id");

        json body;
        std::string error;
        if (!buildBody(request, body, error))
            return fail(std::string(kVendor) + ": " + error, key);

        const HttpHeaders headers = {{"x-goog-api-key", key},
                                     {"content-type", "application/json"}};
        std::string responseBody;
        ChatResponse failure;
        if (!send(post_, joinUrl(baseUrl_, "models/" + model + ":generateContent"), headers,
                  dumpJson(body), kVendor, key, responseBody, failure))
            return failure;
        return parseResponse(responseBody);
    }
    catch (...)
    {
        return fail(std::string(kVendor) + ": unexpected error", key);
    }
}

} // namespace phylo::providers
