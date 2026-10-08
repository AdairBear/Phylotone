// phylo-plughost: runs one plugin in its own process (M4).
//
// The app writes frames to our stdin and reads frames from our stdout (see
// core/include/phylo/host/Wire.h). If the plugin crashes this process dies and
// the app sees the pipe close. The app keeps running.
//
// Limits in this slice: the sample rate is fixed at 48 kHz and the largest block
// is 4096 frames. Both are sent in Hello so the app can check them.

#include "phylo/host/Wire.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <cstdio>
#include <memory>

#if defined(_WIN32)
  #include <fcntl.h>
  #include <io.h>
#else
  #include <unistd.h>
#endif

namespace
{

constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint32_t kMaxBlock = 4096;

void writeFrame(phylo::host::MsgType type, const std::vector<std::uint8_t>& payload)
{
    const auto bytes = phylo::host::encodeFrame(type, payload);
    std::fwrite(bytes.data(), 1, bytes.size(), stdout);
    std::fflush(stdout);
}

void writeError(const std::string& text)
{
    writeFrame(phylo::host::MsgType::Error, phylo::host::encodeText(text));
}

struct Host
{
    juce::AudioPluginFormatManager formats;
    std::unique_ptr<juce::AudioPluginInstance> plugin;

    Host()
    {
        formats.addDefaultFormats();
    }

    // Loads the first plugin found in the file. Returns an empty string on success.
    std::string load(const juce::String& path)
    {
        plugin.reset();
        const juce::File file(path);
        if (!file.exists())
            return "plugin not found: " + path.toStdString();

        for (int i = 0; i < formats.getNumFormats(); ++i)
        {
            auto* format = formats.getFormat(i);
            if (!format->fileMightContainThisPluginType(file.getFullPathName()))
                continue;

            juce::OwnedArray<juce::PluginDescription> types;
            format->findAllTypesForFile(types, file.getFullPathName());
            if (types.isEmpty())
                continue;

            juce::String error;
            plugin = formats.createPluginInstance(*types[0], kSampleRate, kMaxBlock, error);
            if (plugin == nullptr)
                return "could not create plugin: " + error.toStdString();

            plugin->prepareToPlay(kSampleRate, kMaxBlock);
            return {};
        }
        return "no plugin format reads this file: " + path.toStdString();
    }

    void process(const phylo::host::ProcessRequest& req)
    {
        const int channels = static_cast<int>(req.channels);
        const int frames = static_cast<int>(req.frames);

        // Silence is the answer when no plugin is loaded, so the app keeps running.
        std::vector<float> out(static_cast<std::size_t>(req.frames) * req.channels, 0.0f);

        if (plugin != nullptr)
        {
            const int busChannels = std::max({channels, plugin->getTotalNumOutputChannels(),
                                              plugin->getTotalNumInputChannels()});
            const auto offset = [frames](int c) { return static_cast<std::size_t>(c) * static_cast<std::size_t>(frames); };
            juce::AudioBuffer<float> buffer(busChannels, frames);
            buffer.clear();
            for (int c = 0; c < channels; ++c)
                buffer.copyFrom(c, 0, req.audio.data() + offset(c), frames);

            juce::MidiBuffer midi;
            for (const auto& m : req.midi)
            {
                const juce::MidiMessage msg(m.status, m.data1, m.data2);
                midi.addEvent(msg, static_cast<int>(m.frame));
            }

            plugin->processBlock(buffer, midi);

            for (int c = 0; c < channels; ++c)
                std::copy_n(buffer.getReadPointer(c), frames, out.data() + offset(c));
        }

        writeFrame(phylo::host::MsgType::Audio, phylo::host::encodeAudio(req.frames, req.channels, out));
    }
};

} // namespace

int runHost();

int main()
{
    // runHost() has already destroyed the plugin. Skip JUCE's global teardown: it
    // reports leaked singletons for a process that is about to exit anyway.
    const int result = runHost();
    std::fflush(stdout);
    std::_Exit(result);
}

int runHost()
{
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    // Plugins are created and set up on the message thread. This process has no
    // window, so the message manager is created without the GUI.
    juce::MessageManager::getInstance();

    writeFrame(phylo::host::MsgType::Hello,
               phylo::host::encodeHello(phylo::host::kProtocolVersion, kSampleRate, kMaxBlock));

    Host host;
    phylo::host::FrameDecoder decoder;
    std::uint8_t chunk[4096];

    for (;;)
    {
        // read() returns what is available now. fread() would wait for a full buffer,
        // which a live pipe may never deliver.
#if defined(_WIN32)
        const int n = _read(_fileno(stdin), chunk, static_cast<unsigned int>(sizeof chunk));
#else
        const ssize_t n = read(STDIN_FILENO, chunk, sizeof chunk);
#endif
        if (n <= 0)
            return 0; // app closed the pipe

        decoder.feed(chunk, static_cast<std::size_t>(n));

        phylo::host::Frame frame;
        while (decoder.next(frame))
        {
            switch (frame.type)
            {
            case phylo::host::MsgType::Load:
            {
                std::string path;
                if (!phylo::host::decodeText(frame.payload, path))
                {
                    writeError("bad load payload");
                    break;
                }
                const std::string err = host.load(juce::String(juce::CharPointer_UTF8(path.c_str())));
                writeFrame(phylo::host::MsgType::Loaded, phylo::host::encodeText(err));
                break;
            }
            case phylo::host::MsgType::Process:
            {
                phylo::host::ProcessRequest req;
                if (!phylo::host::decodeProcess(frame.payload, req) || req.frames > kMaxBlock || req.channels > 8)
                {
                    writeError("bad process payload");
                    break;
                }
                host.process(req);
                break;
            }
            case phylo::host::MsgType::Shutdown:
                return 0;
            case phylo::host::MsgType::Hello:
            case phylo::host::MsgType::Loaded:
            case phylo::host::MsgType::Audio:
            case phylo::host::MsgType::Error:
            default:
                writeError("unexpected message from app");
                break;
            }
        }

        if (decoder.failed())
        {
            writeError("stream damaged: " + decoder.error());
            return 1;
        }
    }
}

