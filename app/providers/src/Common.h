// Internal helpers shared by the adapters. Not part of the public interface.
#pragma once

#include <phylo/providers/Providers.h>

#include <nlohmann/json.hpp>

#include <string>

namespace phylo::providers::detail
{

using json = nlohmann::json;

// Value of an environment variable, or empty if unset or name is empty.
std::string envValue(const std::string& name);

// Removes `secret` from `text` (replaced with "[redacted]") and caps the length.
std::string sanitise(std::string text, const std::string& secret);

assistant::ChatResponse fail(const std::string& message, const std::string& secret = {});

// Serialises JSON without throwing on invalid UTF-8.
std::string dumpJson(const json& j);

// Parses text into a JSON object. Empty text gives an empty object when
// allowEmpty is set. Returns false if the text is not a JSON object.
bool parseObject(const std::string& text, json& out, bool allowEmpty);

// Joins base and path with exactly one slash.
std::string joinUrl(std::string base, const std::string& path);

// Calls post, catching anything it throws, and maps non-2xx/transport errors.
// On success returns true and fills body; otherwise fills failure.
bool send(const HttpPost& post,
          const std::string& url,
          const HttpHeaders& headers,
          const std::string& body,
          const std::string& vendor,
          const std::string& secret,
          std::string& responseBody,
          assistant::ChatResponse& failure);

// Shared OpenAI chat-completions implementation (OpenAI and compatible servers).
assistant::ChatResponse openAIComplete(const HttpPost& post,
                                       const std::string& baseUrl,
                                       const std::string& key,
                                       const std::string& vendor,
                                       bool useMaxCompletionTokens,
                                       const assistant::ChatRequest& request);

} // namespace phylo::providers::detail
