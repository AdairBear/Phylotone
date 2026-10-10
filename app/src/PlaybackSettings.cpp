#include "PlaybackSettings.h"

#include <cstring>
#include <sstream>

namespace
{
std::string trim(const std::string& s)
{
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return {};
    const auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Values are single-line: a newline would split the record.
std::string oneLine(std::string s)
{
    for (auto& c : s)
        if (c == '\n' || c == '\r')
            c = ' ';
    return trim(s);
}
} // namespace

std::string serialisePlaybackSettings(const PlaybackSettings& s)
{
    return "plughost=" + oneLine(s.pluginHost) + "\nplugin=" + oneLine(s.plugin) + "\nstate=" +
           oneLine(s.state) + "\n";
}

PlaybackSettings parsePlaybackSettings(const std::string& text)
{
    PlaybackSettings out;
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
        if (key == "plughost")
            out.pluginHost = value;
        else if (key == "plugin")
            out.plugin = value;
        else if (key == "state")
            out.state = value;
    }
    return out;
}

PlaybackSettings resolvePlaybackSettings(PlaybackSettings fromFile, const char* envPluginHost,
                                         const char* envPlugin)
{
    if (fromFile.pluginHost.empty() && envPluginHost != nullptr && *envPluginHost != '\0')
        fromFile.pluginHost = envPluginHost;
    if (fromFile.plugin.empty() && envPlugin != nullptr && *envPlugin != '\0')
        fromFile.plugin = envPlugin;
    return fromFile;
}
