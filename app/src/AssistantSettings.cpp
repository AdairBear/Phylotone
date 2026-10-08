#include "AssistantSettings.h"

#include <sstream>

namespace
{
std::string singleLine(std::string s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        if (c != '\r' && c != '\n')
            out += c;
    return out;
}

std::string trim(const std::string& s)
{
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

constexpr ProviderKind kAllProviders[] = { ProviderKind::Anthropic, ProviderKind::OpenAI,
                                           ProviderKind::Gemini, ProviderKind::Local };
} // namespace

const char* providerKey(ProviderKind p) noexcept
{
    switch (p)
    {
    case ProviderKind::Anthropic: return "anthropic";
    case ProviderKind::OpenAI: return "openai";
    case ProviderKind::Gemini: return "gemini";
    case ProviderKind::Local: return "local";
    }
    return "anthropic";
}

bool parseProviderKey(const std::string& s, ProviderKind& out) noexcept
{
    for (auto p : kAllProviders)
    {
        if (s == providerKey(p))
        {
            out = p;
            return true;
        }
    }
    return false;
}

const char* providerDisplayName(ProviderKind p) noexcept
{
    switch (p)
    {
    case ProviderKind::Anthropic: return "Anthropic";
    case ProviderKind::OpenAI: return "OpenAI";
    case ProviderKind::Gemini: return "Gemini";
    case ProviderKind::Local: return "Local (OpenAI-compatible)";
    }
    return "Anthropic";
}

const char* providerKeyEnvName(ProviderKind p) noexcept
{
    switch (p)
    {
    case ProviderKind::Anthropic: return "ANTHROPIC_API_KEY";
    case ProviderKind::OpenAI: return "OPENAI_API_KEY";
    case ProviderKind::Gemini: return "GEMINI_API_KEY";
    case ProviderKind::Local: return "";
    }
    return "";
}

std::string serialiseSettings(const AssistantSettings& s)
{
    std::string out = "# Phylotone assistant settings. API keys are never stored here;\n"
                      "# they are read from the environment.\n";
    out += "provider=" + std::string(providerKey(s.provider)) + "\n";
    for (auto p : kAllProviders)
        out += "model." + std::string(providerKey(p)) + "=" +
               singleLine(s.models[static_cast<size_t>(p)]) + "\n";
    out += "local_base_url=" + singleLine(s.localBaseUrl) + "\n";
    out += "mode=" + std::string(phylo::assistant::modeName(s.mode)) + "\n";
    return out;
}

AssistantSettings parseSettings(const std::string& text)
{
    AssistantSettings s;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
    {
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const auto key = trim(line.substr(0, eq));
        const auto value = trim(line.substr(eq + 1));

        if (key == "provider")
        {
            parseProviderKey(value, s.provider);
        }
        else if (key == "mode")
        {
            phylo::assistant::parseMode(value, s.mode);
        }
        else if (key == "local_base_url")
        {
            if (!value.empty())
                s.localBaseUrl = value;
        }
        else if (key.rfind("model.", 0) == 0)
        {
            ProviderKind p;
            if (parseProviderKey(key.substr(6), p))
                s.models[static_cast<size_t>(p)] = value;
        }
    }
    return s;
}
