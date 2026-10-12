// phylo-plughost: runs one plugin in its own process (M4).
//
// The app writes frames to our stdin and reads frames from our stdout (see
// core/include/phylo/host/Wire.h). If the plugin crashes this process dies and
// the app sees the pipe close. The app keeps running.
//
// Limits in this slice: the sample rate is fixed at 48 kHz and the largest block
// is 4096 frames. Both are sent in Hello so the app can check them.

#include "phylo/host/Fnv1a.h"
#include "phylo/host/Vst3Preset.h"
#include "phylo/host/Wire.h"
#include "phylo/samples/SampleBlob.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
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

constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint32_t kMaxBlock = 4096;

// Frames are written by the message thread and by the audio thread, so a frame is written
// whole under a lock.
std::mutex gWriteMutex;

void writeFrame(phylo::host::MsgType type, const std::vector<std::uint8_t>& payload)
{
    const auto bytes = phylo::host::encodeFrame(type, payload);
    std::lock_guard<std::mutex> lock(gWriteMutex);
    std::fwrite(bytes.data(), 1, bytes.size(), stdout);
    std::fflush(stdout);
}

void writeError(const std::string& text)
{
    writeFrame(phylo::host::MsgType::Error, phylo::host::encodeText(text));
}

// The answer for a sample set the plugin did not take: an old Akazi XL, or any other plugin.
const char kPluginDidNotTake[] = "the plugin did not take the sample set";

// Test hook: PHYLO_HOST_SKIP_BLOB_CHECK=1 skips this host's own check of a sample blob, so a
// bad blob reaches the plugin and the plugin's own refusal can be seen. Not for normal use.
bool skipBlobCheck()
{
    const char* value = std::getenv("PHYLO_HOST_SKIP_BLOB_CHECK");
    return value != nullptr && std::string(value) == "1";
}

// Calls `use` with the plugin's VST3 client, if it has one. The client object only lives for
// the duration of the visit, so all work with it happens inside `use`.
struct Vst3Visitor : juce::ExtensionsVisitor
{
    explicit Vst3Visitor(std::function<void(const VST3Client&)> f) : use(std::move(f)) {}
    void visitVST3Client(const VST3Client& client) override
    {
        visited = true;
        use(client);
    }
    std::function<void(const VST3Client&)> use;
    bool visited = false;
};

// Reads stdin and splits it into frames on its own thread. Process frames go straight to
// `onProcess` (the audio thread's queue); every other frame is queued for the main (message)
// thread. This keeps two things true while the main thread is busy: the plugin's timers and
// async updates can run (for example the one that applies a MIDI Program Change), and a
// Process request is still taken in at once even when such a timer does slow work.
//
// Both queues are bounded, so a fast writer waits (the pipe fills and the app's write waits)
// instead of filling memory: the state-frame queue holds at most about kMaxQueuedBytes of
// payload (checked before a frame is added, so one large frame can exceed it), and the audio
// queue holds at most AudioWorker::kMaxQueued requests. The reader is one thread, so while it
// waits for room in one queue it reads nothing for the other: a backlog of state frames that
// the main thread has not yet taken can hold back a Process frame that arrived after it, for as
// long as the main thread is busy. The app has one request outstanding at a time and waits
// for its answer, so it never builds such a backlog.
class StdinReader
{
public:
    using ProcessSink = std::function<void(phylo::host::Frame&&)>;

    explicit StdinReader(ProcessSink onProcess) : onProcess_(std::move(onProcess)), thread_([this] { run(); })
    {
        thread_.detach();
    }

    enum class Status { Open, Closed, Damaged };

    // Waits up to `waitMs` for frames. Appends the non-Process frames that arrived to `out`.
    // Closed: the app closed the pipe and everything it sent has been handed over. Damaged:
    // the stream is broken (`error` says how) after everything before it has been handed over.
    Status poll(std::vector<phylo::host::Frame>& out, int waitMs, std::string& error)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait_for(lock, std::chrono::milliseconds(waitMs), [this] { return !queue_.empty() || ended_; });
        while (!queue_.empty())
        {
            queuedBytes_ -= queue_.front().payload.size();
            out.push_back(std::move(queue_.front()));
            queue_.pop_front();
        }
        space_.notify_all();
        if (!out.empty() || !ended_)
            return Status::Open;
        error = error_;
        return error_.empty() ? Status::Closed : Status::Damaged;
    }

private:
    static constexpr std::size_t kMaxQueuedBytes = 8u * 1024u * 1024u;

    void run()
    {
        std::vector<std::uint8_t> chunk(64 * 1024);
        phylo::host::FrameDecoder decoder;
        for (;;)
        {
            // read() returns what is available now. fread() would wait for a full buffer,
            // which a live pipe may never deliver.
#if defined(_WIN32)
            const int n = _read(_fileno(stdin), chunk.data(), static_cast<unsigned int>(chunk.size()));
#else
            const ssize_t n = read(STDIN_FILENO, chunk.data(), chunk.size());
#endif
            if (n <= 0)
            {
                end("");
                return;
            }

            decoder.feed(chunk.data(), static_cast<std::size_t>(n));
            phylo::host::Frame frame;
            while (decoder.next(frame))
            {
                if (frame.type == phylo::host::MsgType::Process)
                {
                    onProcess_(std::move(frame));
                    continue;
                }
                std::unique_lock<std::mutex> lock(mutex_);
                space_.wait(lock, [this] { return queuedBytes_ < kMaxQueuedBytes; });
                queuedBytes_ += frame.payload.size();
                queue_.push_back(std::move(frame));
                ready_.notify_all();
            }
            if (decoder.failed())
            {
                end(decoder.error());
                return;
            }
        }
    }

    void end(const std::string& error)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ended_ = true;
        error_ = error;
        ready_.notify_all();
    }

    ProcessSink onProcess_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::condition_variable space_;
    std::deque<phylo::host::Frame> queue_;
    std::size_t queuedBytes_ = 0;
    bool ended_ = false;
    std::string error_;
    std::thread thread_;
};

struct Host
{
    std::mutex pluginMutex;
    juce::AudioPluginFormatManager formats;
    std::unique_ptr<juce::AudioPluginInstance> plugin;

    Host()
    {
        formats.addDefaultFormats();
    }

    // Destroys the plugin before the process leaves. Gives up after kReleaseWaitMs when the
    // audio thread does not let go of the plugin (a render that never returns): the process
    // is leaving either way, so it must not wait on that for ever.
    void release()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kReleaseWaitMs);
        while (!pluginMutex.try_lock())
        {
            if (std::chrono::steady_clock::now() >= deadline)
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        plugin.reset();
        pluginMutex.unlock();
    }

    static constexpr int kReleaseWaitMs = 500;

    // Loads the first plugin found in the file. Returns an empty string on success.
    std::string load(const juce::String& path)
    {
        std::lock_guard<std::mutex> lock(pluginMutex);
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

    // Plugin state as raw bytes, or empty if no plugin is loaded.
    std::vector<std::uint8_t> getState()
    {
        std::vector<std::uint8_t> out;
        if (plugin != nullptr)
        {
            juce::MemoryBlock block;
            plugin->getStateInformation(block);
            const auto* p = static_cast<const std::uint8_t*>(block.getData());
            out.assign(p, p + block.getSize());
        }
        return out;
    }

    // Restores plugin state. Returns an empty string on success.
    std::string setState(const std::vector<std::uint8_t>& bytes)
    {
        if (plugin == nullptr)
            return "no plugin loaded";
        plugin->setStateInformation(bytes.data(), static_cast<int>(bytes.size()));
        return {};
    }

    // Delivers a sample set (docs/SAMPLE_INTERFACE.md, "Delivery" and "The answer") and
    // returns what StateResult carries: empty when the plugin loaded the set, otherwise the
    // plugin's own refusal reason, or kPluginDidNotTake when it did not answer for this set.
    // Takes the blob by value so it can be freed before the plugin decodes it: a full set is
    // 64 MiB and every copy counts.
    //
    // The three attributes decide the answer, not the return value of setPreset: the SDK's
    // loadPreset returns false when the controller step fails after the component has already
    // taken the state. When the plugin did not take the set, its own state from before is put
    // back, so a plugin that does not know this format is not left with a state it never saw
    // the meaning of.
    std::string setSamples(std::vector<std::uint8_t> blob)
    {
        if (plugin == nullptr)
            return "no plugin loaded";

        const std::size_t blobBytes = blob.size();
        const std::string blobHash = phylo::host::fnv1a64Hex(blob.data(), blobBytes);

        // The answer for the plugin's state after the delivery (the Comp chunk, or the raw state).
        const auto answerFrom = [&](const std::vector<std::uint8_t>& state) -> std::string
        {
            const auto xml = juce::AudioProcessor::getXmlFromBinary(state.data(), static_cast<int>(state.size()));
            if (xml == nullptr)
                return kPluginDidNotTake;
            if (xml->getStringAttribute("sampleSetBytes") != juce::String(static_cast<juce::int64>(blobBytes)) ||
                xml->getStringAttribute("sampleSetHash") != juce::String(blobHash))
                return kPluginDidNotTake;
            return xml->getStringAttribute("sampleSetResult").toStdString();
        };

        std::string answer = kPluginDidNotTake;

        // VST3: JUCE's host drops raw bytes given to setStateInformation, so the blob goes in
        // as the component state of a preset container, and the state comes back the same way.
        Vst3Visitor visitor([&](const juce::ExtensionsVisitor::VST3Client& client)
        {
            const juce::MemoryBlock before = client.getPreset();
            phylo::host::Vst3Preset preset;
            if (!phylo::host::parseVst3Preset(static_cast<const std::uint8_t*>(before.getData()), before.getSize(),
                                              preset).ok())
                return;

            {
                // Build the container straight into the block, then free the frame's copy of
                // the blob before the plugin (and JUCE's own copy of the block) takes memory.
                const std::size_t total = phylo::host::vst3PresetSize(blobBytes);
                if (total == 0)
                    return;
                juce::MemoryBlock block(total);
                if (!phylo::host::writeVst3Preset(preset.classId, blob.data(), blobBytes,
                                                  static_cast<std::uint8_t*>(block.getData()), total).ok())
                    return;
                std::vector<std::uint8_t>().swap(blob);
                client.setPreset(block); // the return value is not trusted; see above
            }

            const juce::MemoryBlock after = client.getPreset();
            std::string classId;
            std::vector<std::uint8_t> state;
            if (phylo::host::readVst3Chunk(static_cast<const std::uint8_t*>(after.getData()), after.getSize(),
                                           "Comp", classId, state).ok())
                answer = answerFrom(state);

            if (answer == kPluginDidNotTake)
                client.setPreset(before);
        });
        plugin->getExtensions(visitor);

        if (!visitor.visited)
        {
            // Other formats: raw bytes, as for any plugin state.
            juce::MemoryBlock before;
            plugin->getStateInformation(before);
            plugin->setStateInformation(blob.data(), static_cast<int>(blobBytes));
            std::vector<std::uint8_t>().swap(blob);
            juce::MemoryBlock after;
            plugin->getStateInformation(after);
            const auto* p = static_cast<const std::uint8_t*>(after.getData());
            answer = answerFrom(std::vector<std::uint8_t>(p, p + after.getSize()));
            if (answer == kPluginDidNotTake)
                plugin->setStateInformation(before.getData(), static_cast<int>(before.getSize()));
        }

        return answer;
    }

    // Runs on the audio thread. Holds pluginMutex only against load(), which replaces the
    // plugin; state calls on the message thread run alongside it, as in any host.
    // The answer is written after the lock is let go: a write waits for as long as the app
    // does not read our output, and nothing else may wait on the plugin for that long.
    void process(const phylo::host::ProcessRequest& req)
    {
        std::unique_lock<std::mutex> lock(pluginMutex);
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

        lock.unlock();
        writeFrame(phylo::host::MsgType::Audio, phylo::host::encodeAudio(req.frames, req.channels, out));
    }
};

// Renders on its own thread. The message thread must stay free to run the plugin's timers,
// which can do slow work (for example Akazi XL building a long sample when a Program Change
// selects a slot); a plugin never expects those to hold up its audio, and the app gives up
// on a host that does not answer a block in time.
class AudioWorker
{
public:
    explicit AudioWorker(Host& host) : host_(host), thread_([this] { run(); }) {}

    ~AudioWorker()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        room_.notify_all();
        thread_.join();
    }

    AudioWorker(const AudioWorker&) = delete;
    AudioWorker& operator=(const AudioWorker&) = delete;

    // Blocks the caller (the reader thread) while kMaxQueued requests are waiting, so the pipe
    // pushes back on the app instead of the queue growing without limit.
    void submit(phylo::host::ProcessRequest&& request)
    {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            room_.wait(lock, [this] { return stop_ || queue_.size() < kMaxQueued; });
            queue_.push_back(std::move(request));
        }
        wake_.notify_one();
    }

    static constexpr std::size_t kMaxQueued = 32;

private:
    void run()
    {
        for (;;)
        {
            phylo::host::ProcessRequest request;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
                if (queue_.empty())
                    return; // stopping, and everything submitted has been answered
                request = std::move(queue_.front());
                queue_.pop_front();
                room_.notify_one();
            }
            host_.process(request);
        }
    }

    Host& host_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable room_;
    std::deque<phylo::host::ProcessRequest> queue_;
    bool stop_ = false;
    std::thread thread_;
};

} // namespace

int runHost();

namespace
{
// Leaves the process without unwinding. The reader and audio threads are still running and
// use objects that live in runHost, so those must never be destroyed under them: release the
// plugin (if the audio thread lets go of it in time), flush, and go. (The host has always left through _Exit, to skip JUCE's leak report.)
[[noreturn]] void leave(Host& host, int code)
{
    host.release();
    // Every frame is flushed as it is written, so there is nothing left to flush here unless
    // a writer is in the middle of a frame. If one is (stuck on a full pipe), do not join it.
    if (gWriteMutex.try_lock())
        std::fflush(stdout);
    std::_Exit(code);
}
} // namespace

int main()
{
    // runHost() never returns: it leaves through leave(), which destroys the plugin and
    // exits without unwinding (the reader and audio threads must not outlive their objects).
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
    AudioWorker audio(host); // declared after host: it stops first
    StdinReader input(
        [&audio](phylo::host::Frame&& f)
        {
            phylo::host::ProcessRequest req;
            if (!phylo::host::decodeProcess(f.payload, req) || req.frames > kMaxBlock || req.channels > 8)
            {
                writeError("bad process payload");
                return;
            }
            audio.submit(std::move(req));
        });
    std::vector<phylo::host::Frame> incoming;

    for (;;)
    {
        incoming.clear();
        std::string streamError;
        const auto status = input.poll(incoming, 5, streamError);
        if (status == StdinReader::Status::Closed)
            leave(host, 0); // app closed the pipe
        if (status == StdinReader::Status::Damaged)
        {
            writeError("stream damaged: " + streamError);
            leave(host, 1);
        }

        if (incoming.empty())
        {
            // Nothing to do: let the plugin's timers and async updates run for a moment.
            juce::MessageManager::getInstance()->runDispatchLoopUntil(1);
            continue;
        }

        for (auto& frame : incoming)
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
            case phylo::host::MsgType::GetState:
                writeFrame(phylo::host::MsgType::State, host.getState());
                break;
            case phylo::host::MsgType::SetState:
            {
                // A sample blob is checked before the plugin sees it, and a bad one is refused
                // with its reason even when no plugin is loaded (docs/SAMPLE_INTERFACE.md,
                // "Delivery"). A good one is delivered and the plugin's answer comes back:
                // empty means the plugin loaded it. Other bytes are plugin state and pass
                // through as before.
                std::string refusal;
                if (phylo::samples::looksLikeSampleBlob(frame.payload.data(), frame.payload.size()))
                {
                    if (!skipBlobCheck())
                        refusal = phylo::samples::sampleBlobRefusal(frame.payload.data(), frame.payload.size());
                    if (refusal.empty())
                        refusal = host.setSamples(std::move(frame.payload));
                }
                else
                {
                    refusal = host.setState(frame.payload);
                }
                writeFrame(phylo::host::MsgType::StateResult, phylo::host::encodeText(refusal));
                break;
            }
            case phylo::host::MsgType::Shutdown:
                leave(host, 0);
            case phylo::host::MsgType::Process: // taken in by the reader and rendered on the audio thread
            case phylo::host::MsgType::Hello:
            case phylo::host::MsgType::Loaded:
            case phylo::host::MsgType::Audio:
            case phylo::host::MsgType::Error:
            case phylo::host::MsgType::State:
            case phylo::host::MsgType::StateResult:
            default:
                writeError("unexpected message from app");
                break;
            }
        }
    }
}

