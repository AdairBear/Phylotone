// Tests for the chat-provider adapters, using a fake HttpPost.
// No network: request shapes are checked against hand-written vendor JSON.

#include <phylo/providers/Providers.h>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>

// Portable environment helpers: POSIX setenv/unsetenv, or the MSVC CRT equivalents.
#ifdef _WIN32
static void setEnv(const char* name, const char* value) { _putenv_s(name, value); }
static void unsetEnv(const char* name) { _putenv_s(name, ""); }
#else
static void setEnv(const char* name, const char* value) { setenv(name, value, 1); }
static void unsetEnv(const char* name) { unsetenv(name); }
#endif
#include <memory>
#include <stdexcept>
#include <string>

using json = nlohmann::json;
using namespace phylo::assistant;
using namespace phylo::providers;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                                   \
    do                                                                                \
    {                                                                                 \
        ++g_checks;                                                                   \
        if (!(cond))                                                                  \
        {                                                                             \
            ++g_failures;                                                             \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                             \
    } while (0)

namespace
{
const char* kAnthropicKey = "sk-ant-TESTKEY-anthropic-1234567890";
const char* kOpenAIKey = "sk-TESTKEY-openai-1234567890";
const char* kGeminiKey = "AIzaTESTKEY-gemini-1234567890";
const char* kLocalKey = "TESTKEY-local-1234567890";

struct Fake
{
    std::string url;
    HttpHeaders headers;
    std::string body;
    int calls = 0;
    HttpResult reply;

    HttpPost post()
    {
        return [this](const std::string& u, const HttpHeaders& h, const std::string& b) {
            ++calls;
            url = u;
            headers = h;
            body = b;
            return reply;
        };
    }

    std::string header(const std::string& name) const
    {
        for (const auto& [k, v] : headers)
            if (k == name)
                return v;
        return "<missing>";
    }
    bool hasHeader(const std::string& name) const { return header(name) != "<missing>"; }
};

bool containsAnyKey(const std::string& s)
{
    for (const char* k : {kAnthropicKey, kOpenAIKey, kGeminiKey, kLocalKey})
        if (s.find(k) != std::string::npos)
            return true;
    return false;
}

// A conversation exercising every message kind: system, user, assistant tool
// call, tool result, follow-up user text, and one tool spec.
ChatRequest sampleRequest(const std::string& model, const std::string& callId)
{
    ChatRequest req;
    req.systemPrompt = "You edit scenes.";
    req.model = model;
    req.maxOutputTokens = 512;

    Message u;
    u.role = Role::User;
    u.text = "Make scene 1 faster";
    req.messages.push_back(u);

    Message a;
    a.role = Role::Assistant;
    a.text = "Sure.";
    a.toolCalls.push_back({callId, "edit_scene", R"({"scene":1,"tempo":140})"});
    req.messages.push_back(a);

    Message t;
    t.role = Role::Tool;
    t.toolCallId = callId;
    t.text = R"({"ok":true})";
    req.messages.push_back(t);

    Message u2;
    u2.role = Role::User;
    u2.text = "Thanks";
    req.messages.push_back(u2);

    req.tools.push_back({"edit_scene", "Edit a scene",
                         R"({"type":"object","properties":{"scene":{"type":"integer"},"tempo":{"type":"number"}},"required":["scene"]})"});
    return req;
}

ChatRequest simpleRequest(const std::string& model)
{
    ChatRequest req;
    req.model = model;
    Message u;
    u.text = "hi";
    req.messages.push_back(u);
    return req;
}

// (d) 401 and (e) malformed JSON, shared across adapters. Also a throwing
// transport and a transport error.
void checkFailures(ChatProvider& p, Fake& fake, const std::string& model,
                   const std::string& errorBody)
{
    fake.reply = {401, errorBody, ""};
    ChatResponse r;
    bool threw = false;
    try { r = p.complete(simpleRequest(model)); } catch (...) { threw = true; }
    CHECK(!threw);
    CHECK(!r.ok);
    CHECK(r.error.find("401") != std::string::npos);
    CHECK(!containsAnyKey(r.error));
    std::printf("  [%s] 401 -> \"%s\"\n", p.name().c_str(), r.error.c_str());

    fake.reply = {200, "{\"choices\": [ this is not json", ""};
    threw = false;
    try { r = p.complete(simpleRequest(model)); } catch (...) { threw = true; }
    CHECK(!threw);
    CHECK(!r.ok);
    CHECK(!r.error.empty());
    CHECK(!containsAnyKey(r.error));

    // Valid JSON, wrong shape.
    fake.reply = {200, "[1,2,3]", ""};
    threw = false;
    try { r = p.complete(simpleRequest(model)); } catch (...) { threw = true; }
    CHECK(!threw);
    CHECK(!r.ok);

    fake.reply = {200, "", ""};
    threw = false;
    try { r = p.complete(simpleRequest(model)); } catch (...) { threw = true; }
    CHECK(!threw);
    CHECK(!r.ok);

    fake.reply = {0, "", "connection refused"};
    threw = false;
    try { r = p.complete(simpleRequest(model)); } catch (...) { threw = true; }
    CHECK(!threw);
    CHECK(!r.ok);
    CHECK(r.error.find("connection refused") != std::string::npos);
    CHECK(!containsAnyKey(r.error));

    // 500 with non-JSON body.
    fake.reply = {500, "<html>oops</html>", ""};
    r = p.complete(simpleRequest(model));
    CHECK(!r.ok);
    CHECK(r.error.find("500") != std::string::npos);
}

void testAnthropic()
{
    std::printf("anthropic\n");
    Fake fake;
    AnthropicProvider p(fake.post());
    CHECK(p.name() == "anthropic");

    // (a) request shape
    fake.reply = {200, R"({"content":[{"type":"text","text":"ok"}]})", ""};
    ChatResponse r = p.complete(sampleRequest("claude-sonnet-4-5", "toolu_01"));
    CHECK(r.ok);
    CHECK(fake.url == "https://api.anthropic.com/v1/messages");
    CHECK(fake.header("x-api-key") == kAnthropicKey);
    CHECK(fake.header("anthropic-version") == "2023-06-01");
    CHECK(fake.header("content-type") == "application/json");
    const json b = json::parse(fake.body);
    CHECK(b["model"] == "claude-sonnet-4-5");
    CHECK(b["max_tokens"] == 512);
    CHECK(b["system"] == "You edit scenes.");
    CHECK(b["messages"].size() == 4);
    CHECK(b["messages"][0]["role"] == "user");
    CHECK(b["messages"][0]["content"][0]["type"] == "text");
    CHECK(b["messages"][0]["content"][0]["text"] == "Make scene 1 faster");
    const json& asst = b["messages"][1];
    CHECK(asst["role"] == "assistant");
    CHECK(asst["content"][0]["type"] == "text");
    CHECK(asst["content"][1]["type"] == "tool_use");
    CHECK(asst["content"][1]["id"] == "toolu_01");
    CHECK(asst["content"][1]["name"] == "edit_scene");
    CHECK(asst["content"][1]["input"]["tempo"] == 140);
    CHECK(asst["content"][1]["input"].is_object());
    const json& tr = b["messages"][2];
    CHECK(tr["role"] == "user");
    CHECK(tr["content"][0]["type"] == "tool_result");
    CHECK(tr["content"][0]["tool_use_id"] == "toolu_01");
    CHECK(tr["content"][0]["content"] == R"({"ok":true})");
    CHECK(b["messages"][3]["content"][0]["text"] == "Thanks");
    CHECK(b["tools"].size() == 1);
    CHECK(b["tools"][0]["name"] == "edit_scene");
    CHECK(b["tools"][0]["description"] == "Edit a scene");
    CHECK(b["tools"][0]["input_schema"]["type"] == "object");
    CHECK(b["tools"][0]["input_schema"]["required"][0] == "scene");

    // No system or tools keys when not provided.
    p.complete(simpleRequest("m"));
    const json b2 = json::parse(fake.body);
    CHECK(!b2.contains("system"));
    CHECK(!b2.contains("tools"));

    // (b) text response
    fake.reply = {200,
                  R"({"id":"msg_1","type":"message","role":"assistant","content":[{"type":"text","text":"Hello "},{"type":"text","text":"there"}],"stop_reason":"end_turn"})",
                  ""};
    r = p.complete(simpleRequest("m"));
    CHECK(r.ok);
    CHECK(r.text == "Hello there");
    CHECK(r.toolCalls.empty());

    // (c) tool call response
    fake.reply = {200,
                  R"({"content":[{"type":"text","text":"Calling."},{"type":"tool_use","id":"toolu_abc","name":"edit_scene","input":{"scene":2,"tempo":90.5}}],"stop_reason":"tool_use"})",
                  ""};
    r = p.complete(simpleRequest("m"));
    CHECK(r.ok);
    CHECK(r.text == "Calling.");
    CHECK(r.toolCalls.size() == 1);
    if (!r.toolCalls.empty())
    {
        CHECK(r.toolCalls[0].id == "toolu_abc");
        CHECK(r.toolCalls[0].name == "edit_scene");
        const json args = json::parse(r.toolCalls[0].argsJson);
        CHECK(args["scene"] == 2);
        CHECK(args["tempo"] == 90.5);
    }

    // Bad tool args in history -> ok=false, no throw, no request sent.
    ChatRequest bad = sampleRequest("m", "x");
    bad.messages[1].toolCalls[0].argsJson = "not json";
    const int before = fake.calls;
    r = p.complete(bad);
    CHECK(!r.ok);
    CHECK(fake.calls == before);

    checkFailures(p, fake, "m",
                  R"({"type":"error","error":{"type":"authentication_error","message":"invalid x-api-key"}})");
    CHECK(fake.reply.status == 500);
    fake.reply = {401, R"({"type":"error","error":{"type":"authentication_error","message":"invalid x-api-key"}})", ""};
    r = p.complete(simpleRequest("m"));
    CHECK(r.error.find("invalid x-api-key") != std::string::npos);

    // Vendor message that echoes the key is redacted.
    fake.reply = {401, std::string(R"({"error":{"message":"bad key )") + kAnthropicKey + R"("}})", ""};
    r = p.complete(simpleRequest("m"));
    CHECK(!r.ok);
    CHECK(!containsAnyKey(r.error));
    CHECK(r.error.find("[redacted]") != std::string::npos);

    // Missing key -> no request.
    unsetEnv("ANTHROPIC_API_KEY");
    const int before2 = fake.calls;
    r = p.complete(simpleRequest("m"));
    CHECK(!r.ok);
    CHECK(fake.calls == before2);
    CHECK(r.error.find("ANTHROPIC_API_KEY") != std::string::npos);
    setEnv("ANTHROPIC_API_KEY", kAnthropicKey);
}

void checkOpenAIRequestShape(const json& b, const std::string& model)
{
    CHECK(b["model"] == model);
    CHECK(b["messages"].size() == 5);
    CHECK(b["messages"][0]["role"] == "system");
    CHECK(b["messages"][0]["content"] == "You edit scenes.");
    CHECK(b["messages"][1]["role"] == "user");
    CHECK(b["messages"][1]["content"] == "Make scene 1 faster");
    const json& asst = b["messages"][2];
    CHECK(asst["role"] == "assistant");
    CHECK(asst["content"] == "Sure.");
    CHECK(asst["tool_calls"][0]["id"] == "call_1");
    CHECK(asst["tool_calls"][0]["type"] == "function");
    CHECK(asst["tool_calls"][0]["function"]["name"] == "edit_scene");
    CHECK(asst["tool_calls"][0]["function"]["arguments"].is_string());
    CHECK(asst["tool_calls"][0]["function"]["arguments"] == R"({"scene":1,"tempo":140})");
    const json& tool = b["messages"][3];
    CHECK(tool["role"] == "tool");
    CHECK(tool["tool_call_id"] == "call_1");
    CHECK(tool["content"] == R"({"ok":true})");
    CHECK(b["messages"][4]["content"] == "Thanks");
    CHECK(b["tools"].size() == 1);
    CHECK(b["tools"][0]["type"] == "function");
    CHECK(b["tools"][0]["function"]["name"] == "edit_scene");
    CHECK(b["tools"][0]["function"]["description"] == "Edit a scene");
    CHECK(b["tools"][0]["function"]["parameters"]["properties"]["tempo"]["type"] == "number");
}

void checkOpenAIResponses(ChatProvider& p, Fake& fake)
{
    // (b) text
    fake.reply = {200,
                  R"({"id":"chatcmpl-1","object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":"Hello there"},"finish_reason":"stop"}]})",
                  ""};
    ChatResponse r = p.complete(simpleRequest("m"));
    CHECK(r.ok);
    CHECK(r.text == "Hello there");
    CHECK(r.toolCalls.empty());

    // (c) tool call, content null
    fake.reply = {200,
                  R"({"choices":[{"index":0,"message":{"role":"assistant","content":null,"tool_calls":[{"id":"call_9","type":"function","function":{"name":"edit_scene","arguments":"{\"scene\":3,\"tempo\":128}"}}]},"finish_reason":"tool_calls"}]})",
                  ""};
    r = p.complete(simpleRequest("m"));
    CHECK(r.ok);
    CHECK(r.text.empty());
    CHECK(r.toolCalls.size() == 1);
    if (!r.toolCalls.empty())
    {
        CHECK(r.toolCalls[0].id == "call_9");
        CHECK(r.toolCalls[0].name == "edit_scene");
        CHECK(r.toolCalls[0].argsJson == R"({"scene":3,"tempo":128})");
    }

    // choices empty -> ok=false
    fake.reply = {200, R"({"choices":[]})", ""};
    r = p.complete(simpleRequest("m"));
    CHECK(!r.ok);
}

void testOpenAI()
{
    std::printf("openai\n");
    Fake fake;
    OpenAIProvider p(fake.post());
    CHECK(p.name() == "openai");

    fake.reply = {200, R"({"choices":[{"message":{"role":"assistant","content":"ok"}}]})", ""};
    ChatResponse r = p.complete(sampleRequest("gpt-4.1", "call_1"));
    CHECK(r.ok);
    CHECK(fake.url == "https://api.openai.com/v1/chat/completions");
    CHECK(fake.header("authorization") == std::string("Bearer ") + kOpenAIKey);
    CHECK(fake.header("content-type") == "application/json");
    const json b = json::parse(fake.body);
    checkOpenAIRequestShape(b, "gpt-4.1");
    CHECK(b["max_completion_tokens"] == 512);

    checkOpenAIResponses(p, fake);
    checkFailures(p, fake, "m",
                  R"({"error":{"message":"Incorrect API key provided.","type":"invalid_request_error","code":"invalid_api_key"}})");

    // Custom base URL with trailing slash.
    OpenAIProvider p2(fake.post(), "https://example.test/v1/");
    fake.reply = {200, R"({"choices":[{"message":{"content":"x"}}]})", ""};
    p2.complete(simpleRequest("m"));
    CHECK(fake.url == "https://example.test/v1/chat/completions");

    unsetEnv("OPENAI_API_KEY");
    const int before = fake.calls;
    r = p.complete(simpleRequest("m"));
    CHECK(!r.ok);
    CHECK(fake.calls == before);
    setEnv("OPENAI_API_KEY", kOpenAIKey);
}

void testCompatible()
{
    std::printf("openai-compatible\n");
    Fake fake;

    // No key at all (Ollama).
    OpenAICompatibleProvider p(fake.post(), OpenAICompatibleProvider::kOllamaBaseUrl);
    CHECK(p.name() == "local");
    fake.reply = {200, R"({"choices":[{"message":{"role":"assistant","content":"ok"}}]})", ""};
    ChatResponse r = p.complete(sampleRequest("llama3.1", "call_1"));
    CHECK(r.ok);
    CHECK(fake.url == "http://localhost:11434/v1/chat/completions");
    CHECK(!fake.hasHeader("authorization"));
    const json b = json::parse(fake.body);
    checkOpenAIRequestShape(b, "llama3.1");
    CHECK(b["max_tokens"] == 512);

    checkOpenAIResponses(p, fake);

    // Arguments as an object (seen from some local servers) still yields JSON text.
    fake.reply = {200,
                  R"({"choices":[{"message":{"content":"","tool_calls":[{"function":{"name":"edit_scene","arguments":{"scene":4}}}]}}]})",
                  ""};
    r = p.complete(simpleRequest("m"));
    CHECK(r.ok);
    CHECK(r.toolCalls.size() == 1);
    if (!r.toolCalls.empty())
    {
        CHECK(r.toolCalls[0].id == "local-0");
        CHECK(json::parse(r.toolCalls[0].argsJson)["scene"] == 4);
    }

    // Env name given but unset -> still works without a key.
    unsetEnv("PHYLO_TEST_UNSET_KEY");
    OpenAICompatibleProvider pUnset(fake.post(), OpenAICompatibleProvider::kLMStudioBaseUrl,
                                    "PHYLO_TEST_UNSET_KEY", "lmstudio");
    fake.reply = {200, R"({"choices":[{"message":{"content":"hey"}}]})", ""};
    r = pUnset.complete(simpleRequest("m"));
    CHECK(r.ok);
    CHECK(r.text == "hey");
    CHECK(fake.url == "http://localhost:1234/v1/chat/completions");
    CHECK(!fake.hasHeader("authorization"));

    // With a key.
    OpenAICompatibleProvider pk(fake.post(), "https://router.test/api/v1", "PHYLO_TEST_LOCAL_KEY",
                                "router");
    fake.reply = {200, R"({"choices":[{"message":{"content":"ok"}}]})", ""};
    r = pk.complete(simpleRequest("m"));
    CHECK(r.ok);
    CHECK(fake.header("authorization") == std::string("Bearer ") + kLocalKey);

    checkFailures(pk, fake, "m", R"({"error":{"message":"Unauthorized"}})");
    checkFailures(p, fake, "m", R"({"error":"unauthorized"})");
}

void testGemini()
{
    std::printf("gemini\n");
    Fake fake;
    GeminiProvider p(fake.post());
    CHECK(p.name() == "gemini");

    fake.reply = {200, R"({"candidates":[{"content":{"role":"model","parts":[{"text":"ok"}]}}]})",
                  ""};
    ChatResponse r = p.complete(sampleRequest("gemini-2.5-flash", "gemini-0"));
    CHECK(r.ok);
    CHECK(fake.url
          == "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:generateContent");
    CHECK(fake.url.find(kGeminiKey) == std::string::npos);
    CHECK(fake.url.find("key=") == std::string::npos);
    CHECK(fake.header("x-goog-api-key") == kGeminiKey);
    CHECK(fake.header("content-type") == "application/json");
    const json b = json::parse(fake.body);
    CHECK(!b.contains("model"));
    CHECK(b["system_instruction"]["parts"][0]["text"] == "You edit scenes.");
    CHECK(b["generationConfig"]["maxOutputTokens"] == 512);
    CHECK(b["contents"].size() == 4);
    CHECK(b["contents"][0]["role"] == "user");
    CHECK(b["contents"][0]["parts"][0]["text"] == "Make scene 1 faster");
    const json& model = b["contents"][1];
    CHECK(model["role"] == "model");
    CHECK(model["parts"][0]["text"] == "Sure.");
    CHECK(model["parts"][1]["functionCall"]["name"] == "edit_scene");
    CHECK(model["parts"][1]["functionCall"]["args"]["tempo"] == 140);
    const json& fr = b["contents"][2];
    CHECK(fr["role"] == "user");
    CHECK(fr["parts"][0]["functionResponse"]["name"] == "edit_scene");
    CHECK(fr["parts"][0]["functionResponse"]["response"]["ok"] == true);
    CHECK(b["contents"][3]["parts"][0]["text"] == "Thanks");
    CHECK(b["tools"][0]["functionDeclarations"][0]["name"] == "edit_scene");
    CHECK(b["tools"][0]["functionDeclarations"][0]["description"] == "Edit a scene");
    CHECK(b["tools"][0]["functionDeclarations"][0]["parameters"]["type"] == "object");

    // Non-object tool result text is wrapped.
    ChatRequest plain = sampleRequest("gemini-2.5-flash", "gemini-0");
    plain.messages[2].text = "done";
    p.complete(plain);
    CHECK(json::parse(fake.body)["contents"][2]["parts"][0]["functionResponse"]["response"]["content"]
          == "done");

    // Tool result for an unknown id -> ok=false, no request.
    ChatRequest orphan = sampleRequest("gemini-2.5-flash", "gemini-0");
    orphan.messages[2].toolCallId = "gemini-7";
    int before = fake.calls;
    r = p.complete(orphan);
    CHECK(!r.ok);
    CHECK(fake.calls == before);

    // Unsafe model id is rejected before any request.
    before = fake.calls;
    r = p.complete(simpleRequest("../evil?key=x"));
    CHECK(!r.ok);
    CHECK(fake.calls == before);
    p.complete(simpleRequest("models/gemini-2.5-pro"));
    CHECK(fake.url
          == "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-pro:generateContent");

    // (b) text
    fake.reply = {200,
                  R"({"candidates":[{"content":{"role":"model","parts":[{"text":"thinking...","thought":true},{"text":"Hello "},{"text":"there"}]},"finishReason":"STOP"}]})",
                  ""};
    r = p.complete(simpleRequest("gemini-2.5-flash"));
    CHECK(r.ok);
    CHECK(r.text == "Hello there");
    CHECK(r.toolCalls.empty());

    // (c) two function calls -> synthesised ids
    fake.reply = {200,
                  R"({"candidates":[{"content":{"role":"model","parts":[{"functionCall":{"name":"edit_scene","args":{"scene":5}}},{"functionCall":{"name":"play","args":{}}}]},"finishReason":"STOP"}]})",
                  ""};
    r = p.complete(simpleRequest("gemini-2.5-flash"));
    CHECK(r.ok);
    CHECK(r.toolCalls.size() == 2);
    if (r.toolCalls.size() == 2)
    {
        CHECK(r.toolCalls[0].id == "gemini-0");
        CHECK(r.toolCalls[0].name == "edit_scene");
        CHECK(json::parse(r.toolCalls[0].argsJson)["scene"] == 5);
        CHECK(r.toolCalls[1].id == "gemini-1");
        CHECK(r.toolCalls[1].name == "play");
        CHECK(r.toolCalls[1].argsJson == "{}");
    }

    // Round trip: feed those calls back with two results; both results share one turn.
    ChatRequest rt = simpleRequest("gemini-2.5-flash");
    Message a;
    a.role = Role::Assistant;
    a.toolCalls = r.toolCalls;
    rt.messages.push_back(a);
    for (const auto& tc : r.toolCalls)
    {
        Message t;
        t.role = Role::Tool;
        t.toolCallId = tc.id;
        t.text = "{}";
        rt.messages.push_back(t);
    }
    fake.reply = {200, R"({"candidates":[{"content":{"parts":[{"text":"ok"}]}}]})", ""};
    r = p.complete(rt);
    CHECK(r.ok);
    const json rb = json::parse(fake.body);
    CHECK(rb["contents"].size() == 3);
    CHECK(rb["contents"][1]["parts"].size() == 2); // no empty text part
    CHECK(rb["contents"][2]["parts"].size() == 2);
    CHECK(rb["contents"][2]["parts"][0]["functionResponse"]["name"] == "edit_scene");
    CHECK(rb["contents"][2]["parts"][1]["functionResponse"]["name"] == "play");

    // Blocked prompt -> ok=false
    fake.reply = {200, R"({"promptFeedback":{"blockReason":"SAFETY"}})", ""};
    r = p.complete(simpleRequest("gemini-2.5-flash"));
    CHECK(!r.ok);
    CHECK(r.error.find("SAFETY") != std::string::npos);

    checkFailures(p, fake, "gemini-2.5-flash",
                  R"({"error":{"code":401,"message":"API key not valid. Please pass a valid API key.","status":"UNAUTHENTICATED"}})");

    unsetEnv("GEMINI_API_KEY");
    before = fake.calls;
    r = p.complete(simpleRequest("gemini-2.5-flash"));
    CHECK(!r.ok);
    CHECK(fake.calls == before);
    setEnv("GEMINI_API_KEY", kGeminiKey);
}

void testTransportEdgeCases()
{
    std::printf("transport\n");
    // Empty HttpPost.
    OpenAIProvider empty{HttpPost{}};
    ChatResponse r = empty.complete(simpleRequest("m"));
    CHECK(!r.ok);

    // Throwing transport whose message contains the key.
    AnthropicProvider thrower([](const std::string&, const HttpHeaders& h, const std::string&) -> HttpResult {
        throw std::runtime_error("boom with header " + h.front().second);
    });
    bool threw = false;
    try { r = thrower.complete(simpleRequest("m")); } catch (...) { threw = true; }
    CHECK(!threw);
    CHECK(!r.ok);
    CHECK(!containsAnyKey(r.error));

    // responseFromHttp helper directly.
    r = responseFromHttp({200, "{}", ""}, "x");
    CHECK(r.ok);
    r = responseFromHttp({403, R"({"error":{"message":"forbidden"}})", ""}, "x");
    CHECK(!r.ok);
    CHECK(r.error == "x: HTTP 403: forbidden");
    r = responseFromHttp({0, "", std::string("timeout ") + kOpenAIKey}, "x", kOpenAIKey);
    CHECK(!r.ok);
    CHECK(!containsAnyKey(r.error));
    r = responseFromHttp({502, std::string(5000, 'a'), ""}, "x");
    CHECK(r.error == "x: HTTP 502");
}
} // namespace

int main()
{
    setEnv("ANTHROPIC_API_KEY", kAnthropicKey);
    setEnv("OPENAI_API_KEY", kOpenAIKey);
    setEnv("GEMINI_API_KEY", kGeminiKey);
    setEnv("PHYLO_TEST_LOCAL_KEY", kLocalKey);

    testAnthropic();
    testOpenAI();
    testCompatible();
    testGemini();
    testTransportEdgeCases();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
