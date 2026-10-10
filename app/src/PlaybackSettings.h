// Plugin playback settings: where the plugin host and the plugin are (M4).
//
// Stored in a plain key=value file next to the assistant settings. When a value is empty
// in the file, the environment is used: PHYLO_PLUGHOST and PHYLO_PLUGIN. The file wins
// over the environment. No JUCE dependency, so it can be checked headlessly.
#pragma once

#include <string>

struct PlaybackSettings
{
    std::string pluginHost; // path to phylo-plughost
    std::string plugin;     // path to the plugin (a .vst3 bundle)
    std::string state;      // optional: file of raw plugin state bytes, restored on start

    bool complete() const noexcept { return !pluginHost.empty() && !plugin.empty(); }
};

// key=value lines, values single-line.
std::string serialisePlaybackSettings(const PlaybackSettings& s);

// Keys: plughost, plugin, state. Unknown keys, blank lines and lines starting with '#' are ignored.
PlaybackSettings parsePlaybackSettings(const std::string& text);

// Fills any empty value from the environment. Values from the file are kept.
PlaybackSettings resolvePlaybackSettings(PlaybackSettings fromFile, const char* envPluginHost,
                                         const char* envPlugin);
