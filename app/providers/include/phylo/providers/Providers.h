// Chat-provider adapters for the assistant (M2).
//
// Each adapter implements phylo::assistant::ChatProvider and builds the vendor's
// wire format. Networking is injected as an HttpPost function so the adapters do
// not depend on any HTTP library and can be tested with a fake transport.
//
// Keys are read from the environment at the time of each complete() call and are
// never placed in URLs or error strings.

#pragma once

#include <phylo/assistant/ChatProvider.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace phylo::providers
{

struct HttpResult
{
    int status = 0;     // HTTP status; 0 when the request never got a response
    std::string body;   // response body
    std::string error;  // transport error text when status == 0
};

using HttpHeaders = std::vector<std::pair<std::string, std::string>>;

using HttpPost = std::function<HttpResult(const std::string& url,
                                          const HttpHeaders& headers,
                                          const std::string& body)>;

// Wraps a non-success HttpResult into a failed ChatResponse. Uses the status and,
// when the body carries one, the vendor's error message. Never includes headers.
// Any occurrence of `secret` (if non-empty) is redacted from the message.
// Returns a response with ok == true and nothing else set when result is 2xx.
assistant::ChatResponse responseFromHttp(const HttpResult& result,
                                         const std::string& vendor,
                                         const std::string& secret = {});

// https://api.anthropic.com/v1/messages, key from ANTHROPIC_API_KEY.
class AnthropicProvider : public assistant::ChatProvider
{
public:
    explicit AnthropicProvider(HttpPost post,
                               std::string baseUrl = "https://api.anthropic.com/v1");
    std::string name() const override { return "anthropic"; }
    assistant::ChatResponse complete(const assistant::ChatRequest& request) override;

private:
    HttpPost post_;
    std::string baseUrl_;
};

// {baseUrl}/chat/completions, key from OPENAI_API_KEY.
class OpenAIProvider : public assistant::ChatProvider
{
public:
    explicit OpenAIProvider(HttpPost post,
                            std::string baseUrl = "https://api.openai.com/v1");
    std::string name() const override { return "openai"; }
    assistant::ChatResponse complete(const assistant::ChatRequest& request) override;

private:
    HttpPost post_;
    std::string baseUrl_;
};

// {baseUrl}/models/{model}:generateContent, key from GEMINI_API_KEY (header only).
class GeminiProvider : public assistant::ChatProvider
{
public:
    explicit GeminiProvider(HttpPost post,
                            std::string baseUrl =
                                "https://generativelanguage.googleapis.com/v1beta");
    std::string name() const override { return "gemini"; }
    assistant::ChatResponse complete(const assistant::ChatRequest& request) override;

private:
    HttpPost post_;
    std::string baseUrl_;
};

// Any server speaking the OpenAI chat-completions format. The key is optional:
// pass an empty keyEnvName (or leave that variable unset) for local servers.
//   Ollama:    http://localhost:11434/v1
//   LM Studio: http://localhost:1234/v1
class OpenAICompatibleProvider : public assistant::ChatProvider
{
public:
    static constexpr const char* kOllamaBaseUrl = "http://localhost:11434/v1";
    static constexpr const char* kLMStudioBaseUrl = "http://localhost:1234/v1";

    OpenAICompatibleProvider(HttpPost post,
                             std::string baseUrl,
                             std::string keyEnvName = {},
                             std::string displayName = "local");
    std::string name() const override { return displayName_; }
    assistant::ChatResponse complete(const assistant::ChatRequest& request) override;

private:
    HttpPost post_;
    std::string baseUrl_;
    std::string keyEnvName_;
    std::string displayName_;
};

} // namespace phylo::providers
