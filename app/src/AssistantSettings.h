// Non-secret assistant settings and their plain key=value file format.
//
// Only the provider, model names, local base URL and mode are stored. API keys
// are never stored: the provider adapters read them from the environment at
// call time. This file has no JUCE dependency so it can be checked headlessly.

#pragma once

#include "phylo/assistant/Assistant.h"

#include <array>
#include <string>

enum class ProviderKind { Anthropic = 0, OpenAI = 1, Gemini = 2, Local = 3 };

constexpr int kNumProviderKinds = 4;

// Stable key used in the settings file: "anthropic", "openai", "gemini", "local".
const char* providerKey(ProviderKind p) noexcept;
bool parseProviderKey(const std::string& s, ProviderKind& out) noexcept;

// Display name for the provider picker.
const char* providerDisplayName(ProviderKind p) noexcept;

// Environment variable the adapter reads its key from; empty for Local.
const char* providerKeyEnvName(ProviderKind p) noexcept;

struct AssistantSettings
{
    static constexpr const char* kDefaultLocalBaseUrl = "http://localhost:11434/v1";
    static constexpr const char* kDefaultAnthropicModel = "claude-sonnet-5-5";

    ProviderKind provider = ProviderKind::Anthropic;

    // One model name per provider, so switching provider restores its model.
    // Only Anthropic has a default; the others must be chosen by the user.
    std::array<std::string, kNumProviderKinds> models { kDefaultAnthropicModel, "", "", "" };

    std::string localBaseUrl = kDefaultLocalBaseUrl;
    phylo::assistant::Mode mode = phylo::assistant::Mode::Ask;

    const std::string& model() const { return models[static_cast<size_t>(provider)]; }
    std::string& model() { return models[static_cast<size_t>(provider)]; }
};

// Writes the settings as key=value lines. Values are single-line (newlines are removed).
std::string serialiseSettings(const AssistantSettings& s);

// Reads key=value lines. Unknown keys, blank lines and lines starting with '#'
// are ignored; invalid values keep the defaults. Never fails.
AssistantSettings parseSettings(const std::string& text);
