// A stand-in plugin host for the sample send check. Plain C++, no JUCE.
//
// Speaks the phylo-plughost wire protocol on stdin/stdout and answers SetState the way
// docs/SAMPLE_INTERFACE.md asks of the plugin side: a sample blob is validated, and a bad
// one is refused with its reason while the current slots stay. Anything else is taken as
// the plugin's own state. This is test machinery, not a plugin.
//
// So the check can see what the host holds, every sample of every Process answer is
//   (number of loaded slots) + 1000 if a non-blob state has been restored.
//
// Misbehaviour the check can ask for through environment variables (children inherit them):
//   PHYLO_FAKE_DIE_ON_SLOTS=N    exit when a valid blob with N slots arrives
//   PHYLO_FAKE_STALL_ON_SLOTS=N  stop answering when a valid blob with N slots arrives
//   PHYLO_FAKE_NEVER_READ=1      send Hello, then never read anything again
//   PHYLO_FAKE_EXTRA_RESULT=1    send one extra empty StateResult after each refusal
//   PHYLO_FAKE_LONG_REASON=1     refuse a bad blob with a very long reason
//   PHYLO_FAKE_REFUSE_STATE=1    refuse non-blob state bytes with "state not wanted"

#include "phylo/host/Wire.h"
#include "phylo/samples/SampleBlob.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
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

// The number in an environment variable, or -1 when it is not set.
long envNumber(const char* name)
{
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr)
        return -1;
    const long n = std::strtol(value, nullptr, 10);
    std::free(value);
    return n;
#else
    const char* value = std::getenv(name);
    return value == nullptr ? -1 : std::strtol(value, nullptr, 10);
#endif
}

void sleepFor(int seconds)
{
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
}
} // namespace

int main()
{
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    const long dieOnSlots = envNumber("PHYLO_FAKE_DIE_ON_SLOTS");
    const long stallOnSlots = envNumber("PHYLO_FAKE_STALL_ON_SLOTS");
    const bool extraResult = envNumber("PHYLO_FAKE_EXTRA_RESULT") > 0;
    const bool longReason = envNumber("PHYLO_FAKE_LONG_REASON") > 0;
    const bool refuseState = envNumber("PHYLO_FAKE_REFUSE_STATE") > 0;

    writeFrame(MsgType::Hello, phylo::host::encodeHello(phylo::host::kProtocolVersion, 48000, 4096));

    if (envNumber("PHYLO_FAKE_NEVER_READ") > 0)
    {
        sleepFor(120); // the app kills this process long before
        return 0;
    }

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
                    {
                        const long count = static_cast<long>(decoded.size());
                        if (count == dieOnSlots)
                            return 3; // a blob that kills the host
                        if (count == stallOnSlots)
                        {
                            sleepFor(120); // a blob that stalls it: no answer, the app gives up
                            return 0;
                        }
                        slots = std::move(decoded); // the blob replaces every slot
                    }
                    else
                    {
                        reason = result.message; // refused: the current slots stay
                        if (longReason)
                        {
                            // One ASCII letter, then two-byte characters: byte 200 is the second half
                            // of a character, so a cut there must move back.
                            reason = "x";
                            for (int i = 0; i < 400; ++i)
                                reason += "\xc3\xa9";
                        }
                    }
                }
                else if (refuseState)
                {
                    reason = "state not wanted";
                }
                else
                {
                    stateRestored = true;
                }
                writeFrame(MsgType::StateResult, phylo::host::encodeText(reason));
                if (extraResult && !reason.empty())
                    writeFrame(MsgType::StateResult, phylo::host::encodeText("")); // a stray "accepted"
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
