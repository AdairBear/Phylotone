// A stand-in plugin host for the sample send check. Plain C++, no JUCE.
//
// Speaks the phylo-plughost wire protocol on stdin/stdout and answers SetState the way
// docs/SAMPLE_INTERFACE.md asks of the plugin side: a sample blob is validated, and a bad
// one is refused with its reason while the current slots stay. Anything else is taken as
// the plugin's own state. This is test machinery, not a plugin.
//
// So the check can see what the host holds, every sample of every Process answer is
//   (number of loaded slots) + 1000 if a non-blob state has been restored.

#include "phylo/host/Wire.h"
#include "phylo/samples/SampleBlob.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#if defined(_WIN32)
  #include <fcntl.h>
  #include <io.h>
#else
  #include <unistd.h>
#endif

namespace
{
using phylo::host::MsgType;

void writeFrame(MsgType type, const std::vector<std::uint8_t>& payload)
{
    const auto bytes = phylo::host::encodeFrame(type, payload);
    std::fwrite(bytes.data(), 1, bytes.size(), stdout);
    std::fflush(stdout);
}
} // namespace

int main()
{
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    writeFrame(MsgType::Hello, phylo::host::encodeHello(phylo::host::kProtocolVersion, 48000, 4096));

    phylo::samples::SampleSlotSet slots; // what the "plugin" holds
    bool stateRestored = false;
    phylo::host::FrameDecoder decoder;
    std::uint8_t chunk[4096];

    for (;;)
    {
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
            case MsgType::Load:
                writeFrame(MsgType::Loaded, phylo::host::encodeText("")); // always "loaded"
                break;
            case MsgType::SetState:
            {
                const auto& bytes = frame.payload;
                std::string reason;
                if (phylo::samples::looksLikeSampleBlob(bytes.data(), bytes.size()))
                {
                    phylo::samples::SampleSlotSet decoded;
                    const auto result = phylo::samples::decodeSampleBlob(bytes.data(), bytes.size(), decoded);
                    if (result.ok())
                        slots = std::move(decoded); // the blob replaces every slot
                    else
                        reason = result.message; // refused: the current slots stay
                }
                else
                {
                    stateRestored = true;
                }
                writeFrame(MsgType::StateResult, phylo::host::encodeText(reason));
                break;
            }
            case MsgType::Process:
            {
                phylo::host::ProcessRequest req;
                if (!phylo::host::decodeProcess(frame.payload, req))
                {
                    writeFrame(MsgType::Error, phylo::host::encodeText("bad process payload"));
                    break;
                }
                const float level = static_cast<float>(slots.size()) + (stateRestored ? 1000.0f : 0.0f);
                const std::vector<float> out(static_cast<std::size_t>(req.frames) * req.channels, level);
                writeFrame(MsgType::Audio, phylo::host::encodeAudio(req.frames, req.channels, out));
                break;
            }
            case MsgType::GetState:
                writeFrame(MsgType::State, {});
                break;
            case MsgType::Shutdown:
                return 0;
            default:
                writeFrame(MsgType::Error, phylo::host::encodeText("unexpected message from app"));
                break;
            }
        }

        if (decoder.failed())
            return 1; // stream damaged
    }
}
