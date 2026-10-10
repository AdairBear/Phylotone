// Headless check of the playback settings file and environment fallback (M4). Plain C++.
#include "PlaybackSettings.h"

#include <cstdio>

namespace
{
int failures = 0;

#define CHECK(cond)                                                             \
    do                                                                          \
    {                                                                           \
        if (!(cond))                                                            \
        {                                                                       \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);                 \
            ++failures;                                                         \
        }                                                                       \
    } while (0)
} // namespace

int main()
{
    // Round trip.
    PlaybackSettings s { "/opt/phylo-plughost", "/Library/Audio/Plug-Ins/VST3/Akazi XL.vst3" };
    const auto back = parsePlaybackSettings(serialisePlaybackSettings(s));
    CHECK(back.pluginHost == s.pluginHost);
    CHECK(back.plugin == s.plugin);
    CHECK(back.complete());

    // The state file path round-trips and is optional.
    PlaybackSettings withState = s;
    withState.state = "/Users/me/Documents/Phylotone/akazi.state";
    CHECK(parsePlaybackSettings(serialisePlaybackSettings(withState)).state == withState.state);
    CHECK(parsePlaybackSettings("plughost=/h\nplugin=/p\n").state.empty());

    // Comments, blank lines, unknown keys and spacing are tolerated.
    const auto messy = parsePlaybackSettings("# note\n\n  plughost =  /a/host  \nother=1\nplugin=/p.vst3\n");
    CHECK(messy.pluginHost == "/a/host");
    CHECK(messy.plugin == "/p.vst3");

    // A newline in a value cannot split the record.
    const auto nl = parsePlaybackSettings(serialisePlaybackSettings({ "/h\nplugin=/evil", "" }));
    CHECK(nl.plugin.empty());
    CHECK(!nl.complete());

    // Environment fills what the file leaves empty.
    const auto fromEnv = resolvePlaybackSettings({}, "/env/host", "/env/plugin.vst3");
    CHECK(fromEnv.pluginHost == "/env/host");
    CHECK(fromEnv.plugin == "/env/plugin.vst3");
    CHECK(fromEnv.complete());

    // The file wins over the environment.
    const auto fileWins = resolvePlaybackSettings({ "/file/host", "" }, "/env/host", "/env/plugin.vst3");
    CHECK(fileWins.pluginHost == "/file/host");
    CHECK(fileWins.plugin == "/env/plugin.vst3");

    // Nothing set anywhere: not complete, no crash on null environment.
    const auto none = resolvePlaybackSettings({}, nullptr, "");
    CHECK(!none.complete());

    if (failures == 0)
        std::printf("playback settings check: all passed\n");
    else
        std::printf("playback settings check: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
