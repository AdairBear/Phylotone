// Headless check of the audio feed (M4). Plain C++, no JUCE.
//
// Usage: phylo_audio_feed_check
//
// Uses a synchronous fake sink, so the checks are about the feed's own logic:
//  - pulling in odd-sized chunks gives the same audio as pulling in one go,
//  - the feed's output matches renderPatternOffline with the same block size,
//  - a block the sink refuses becomes silence, and the stream keeps its length,
//  - after stop(), no new note-ons reach the sink.

#include "AudioFeed.h"
#include "OfflineTake.h"

#include <cmath>
#include <cstdio>
#include <deque>

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

// Renders each block to a marker: the first frame's left sample is 1 + the MIDI count,
// so silence means the block was not rendered, and a block with notes is loud at its start.
class FakeSink : public AudioBlockSink
{
public:
    explicit FakeSink(std::size_t refuseEvery = 0) : refuseEvery_(refuseEvery) {}

    bool submit(phylo::host::ProcessRequest block) override
    {
        ++submits_;
        if (refuseEvery_ != 0 && submits_ % refuseEvery_ == 0)
            return false;
        for (const auto& m : block.midi)
            if ((m.status & 0xF0) == 0x90 && m.data2 > 0)
                ++noteOns_;
        std::vector<float> out(static_cast<std::size_t>(block.frames) * 2, 0.0f);
        out[0] = 1.0f + static_cast<float>(block.midi.size());
        done_.push_back(std::move(out));
        return true;
    }

    bool take(std::vector<float>& out, std::size_t samples) override
    {
        if (done_.empty())
        {
            out.assign(samples, 0.0f);
            return false;
        }
        out = std::move(done_.front());
        done_.pop_front();
        out.resize(samples, 0.0f);
        return true;
    }

    std::size_t noteOns() const { return noteOns_; }
    void resetNoteOns() { noteOns_ = 0; }

private:
    std::size_t refuseEvery_;
    std::size_t submits_ = 0;
    std::size_t noteOns_ = 0;
    std::deque<std::vector<float>> done_;
};

phylo::Pattern pattern()
{
    phylo::Pattern p(4.0);
    for (int b = 0; b < 4; ++b)
        p.addNote(b, 60 + b, 100, 0.5);
    return p;
}

constexpr double kRate = 48000.0;
constexpr double kBpm = 120.0;
constexpr std::size_t kBlock = 512;
constexpr std::size_t kTotal = kBlock * 6; // six blocks, 0.064 s each

// The same marker rule as FakeSink, used to compare with the offline render.
BlockRenderer markerRender()
{
    return [](const phylo::host::ProcessRequest& req, std::vector<float>& out) {
        out.assign(static_cast<std::size_t>(req.frames) * 2, 0.0f);
        out[0] = 1.0f + static_cast<float>(req.midi.size());
    };
}

// Pulls `total` frames in chunks of `chunk`, returns interleaved stereo.
std::vector<float> pullAll(AudioFeed& feed, std::size_t total, std::size_t chunk)
{
    std::vector<float> out;
    std::vector<float> l(chunk), r(chunk);
    for (std::size_t done = 0; done < total; done += chunk)
    {
        const std::size_t n = std::min(chunk, total - done);
        feed.pull(l.data(), r.data(), n);
        for (std::size_t i = 0; i < n; ++i)
        {
            out.push_back(l[i]);
            out.push_back(r[i]);
        }
    }
    return out;
}
} // namespace

int main()
{
    // 1. Odd-sized pulls give the same audio as one long pull.
    FakeSink sinkA;
    AudioFeed feedA(sinkA, kRate, kBlock);
    feedA.setTempo(kBpm);
    feedA.setPattern(pattern());
    feedA.play();
    const auto oddChunks = pullAll(feedA, kTotal, 100);

    FakeSink sinkB;
    AudioFeed feedB(sinkB, kRate, kBlock);
    feedB.setTempo(kBpm);
    feedB.setPattern(pattern());
    feedB.play();
    const auto oneGo = pullAll(feedB, kTotal, kTotal);

    CHECK(oddChunks.size() == kTotal * 2);
    CHECK(oddChunks == oneGo);

    // 2. Matches the offline render with the same block size.
    phylo::Pattern p = pattern();
    const auto offline = renderPatternOffline(p, kBpm, static_cast<int>(kRate), kTotal, markerRender(), kBlock);
    CHECK(offline.stereo == oneGo);

    // 3. A refused block (every third submit) is silence and the length is kept.
    FakeSink sinkC(3);
    AudioFeed feedC(sinkC, kRate, kBlock);
    feedC.setTempo(kBpm);
    feedC.setPattern(pattern());
    feedC.play();
    const auto withDrops = pullAll(feedC, kTotal, 256);
    CHECK(withDrops.size() == kTotal * 2);
    CHECK(feedC.droppedBlocks() == 2); // submits 3 and 6
    // Blocks 2 and 5 (0-based) are the dropped ones; their first sample is silence.
    CHECK(withDrops[2 * kBlock * 2] == 0.0f);
    CHECK(withDrops[5 * kBlock * 2] == 0.0f);
    CHECK(withDrops[0] != 0.0f); // a block that was rendered is not silence

    // 4. After stop(), no new note-ons reach the sink.
    FakeSink sinkD;
    AudioFeed feedD(sinkD, kRate, kBlock);
    feedD.setTempo(kBpm);
    feedD.setPattern(pattern());
    feedD.play();
    // Just under two seconds: the four notes have sounded, and the loop's restart at
    // 96000 frames has not. Blocks are generated whole, so the window stays short of it.
    pullAll(feedD, 95000, kBlock);
    const std::size_t before = sinkD.noteOns();
    CHECK(before == 4);
    feedD.stop();
    pullAll(feedD, kTotal * 4, kBlock);
    CHECK(sinkD.noteOns() == before);

    if (failures == 0)
        std::printf("audio feed check: all passed\n");
    else
        std::printf("audio feed check: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
