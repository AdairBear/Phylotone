// Headless check of the assistant settings file format. Exits non-zero on failure.

#include "AssistantSettings.h"

#include <cstdio>
#include <string>

namespace
{
int failures = 0;

void check(bool ok, const char* what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}
} // namespace

int main()
{
    using phylo::assistant::Mode;

    // Defaults.
    {
        const AssistantSettings d = parseSettings("");
        check(d.provider == ProviderKind::Anthropic, "default provider is Anthropic");
        check(d.mode == Mode::Ask, "default mode is Ask");
        check(d.localBaseUrl == "http://localhost:11434/v1", "default local base URL");
        check(d.model() == AssistantSettings::kDefaultAnthropicModel, "Anthropic has a default model");
        check(d.models[1].empty() && d.models[2].empty() && d.models[3].empty(),
              "no default model for OpenAI, Gemini, Local");
    }

    // Round trip.
    {
        AssistantSettings s;
        s.provider = ProviderKind::Local;
        s.models[0] = "a-model";
        s.models[1] = "o-model";
        s.models[2] = "g-model";
        s.models[3] = "llama3.1:8b";
        s.localBaseUrl = "http://localhost:1234/v1";
        s.mode = Mode::Assist;

        const auto text = serialiseSettings(s);
        const auto r = parseSettings(text);
        check(r.provider == ProviderKind::Local, "round trip provider");
        check(r.models == s.models, "round trip models");
        check(r.localBaseUrl == s.localBaseUrl, "round trip base URL");
        check(r.mode == Mode::Assist, "round trip mode");
        check(serialiseSettings(r) == text, "round trip is stable");
        check(text.find("API_KEY") == std::string::npos && text.find("api_key") == std::string::npos,
              "no key field is written");
    }

    // Newlines in values cannot inject extra keys.
    {
        AssistantSettings s;
        s.models[0] = "x\nprovider=gemini";
        const auto r = parseSettings(serialiseSettings(s));
        check(r.provider == ProviderKind::Anthropic, "newline in value does not inject a key");
    }

    // Unknown keys, comments, junk and bad values are ignored.
    {
        const auto r = parseSettings("# comment\n\njunk\nprovider=nope\nmode=loud\n"
                                     "anthropic_api_key=sk-secret\nmodel.openai = gpt-x \r\n"
                                     "local_base_url=\n");
        check(r.provider == ProviderKind::Anthropic, "bad provider keeps default");
        check(r.mode == Mode::Ask, "bad mode keeps default");
        check(r.models[1] == "gpt-x", "values are trimmed, CRLF tolerated");
        check(r.localBaseUrl == "http://localhost:11434/v1", "empty base URL keeps default");
        check(serialiseSettings(r).find("sk-secret") == std::string::npos,
              "a key found in the file is not written back");
    }

    if (failures == 0)
        std::printf("assistant settings checks passed\n");
    return failures == 0 ? 0 : 1;
}
