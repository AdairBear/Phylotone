#include "AudioFeed.h"
#include "OfflineTake.h"

#include <memory>
#include <utility>

AudioFeed::AudioFeed(AudioBlockSink& sink, double sampleRate, std::size_t blockFrames)
    : sink_(sink),
      blockFrames_(blockFrames > 0 ? blockFrames : 1),
      sampleRate_(sampleRate),
      tempo_(120.0),
      meter_(4)
{
}

AudioFeed::~AudioFeed()
{
    delete pendingPattern_.exchange(nullptr);
}

void AudioFeed::setSampleRate(double sampleRate)
{
    sampleRate_.store(sampleRate, std::memory_order_release);
}

void AudioFeed::setTempo(double bpm)
{
    tempo_.store(bpm, std::memory_order_release);
}

void AudioFeed::setMeter(int beatsPerBar)
{
    meter_.store(beatsPerBar, std::memory_order_release);
}

void AudioFeed::setPattern(phylo::Pattern pattern)
{
    // Publish the newest pattern. The previous unclaimed one is freed here, on the
    // message thread, so the audio thread never frees a pattern it did not take.
    auto* incoming = new phylo::Pattern(std::move(pattern));
    delete pendingPattern_.exchange(incoming, std::memory_order_acq_rel);
}

void AudioFeed::play()
{
    wantPlaying_.store(true, std::memory_order_release);
}

void AudioFeed::stop()
{
    wantPlaying_.store(false, std::memory_order_release);
}

std::size_t AudioFeed::droppedBlocks() const
{
    return dropped_.load(std::memory_order_acquire);
}

void AudioFeed::applyControls()
{
    const double rate = sampleRate_.load(std::memory_order_acquire);
    if (rate != appliedSampleRate_)
    {
        seq_.setSampleRate(rate);
        appliedSampleRate_ = rate;
    }

    const double bpm = tempo_.load(std::memory_order_acquire);
    if (bpm != appliedTempo_)
    {
        seq_.setTempo(bpm);
        appliedTempo_ = bpm;
    }

    const int meter = meter_.load(std::memory_order_acquire);
    if (meter != appliedMeter_)
    {
        seq_.setMeter(meter);
        appliedMeter_ = meter;
    }

    // A pattern is taken before the transport changes, so a pattern and a play in the same
    // pull start on the new pattern.
    //
    // Note: the sequencer destroys the pattern it replaces, so the note storage of the old
    // pattern is freed here, on the audio thread. The control values above are lock-free;
    // this free and the sink's calls are the parts still to move off the audio thread.
    if (phylo::Pattern* incoming = pendingPattern_.exchange(nullptr, std::memory_order_acq_rel))
    {
        std::unique_ptr<phylo::Pattern> owned(incoming);
        if (seq_.isPlaying())
            seq_.scheduleNextBar(std::move(*owned));
        else
            seq_.setPattern(std::move(*owned));
    }

    const bool wantPlaying = wantPlaying_.load(std::memory_order_acquire);
    if (wantPlaying != seq_.isPlaying())
    {
        if (wantPlaying)
            seq_.play();
        else
            seq_.stop();
    }
}

void AudioFeed::pull(float* left, float* right, std::size_t frames)
{
    applyControls();

    // Make blocks until there is enough output. Each block is submitted once and taken
    // once, or counted as dropped and filled with silence, so the stream keeps its length.
    while (ready_.size() < frames * 2)
    {
        midi_.clear();
        seq_.process(static_cast<int>(blockFrames_), midi_);

        if (sink_.submit(buildBlock(midi_, blockFrames_)))
        {
            sink_.take(block_, blockFrames_ * 2);
        }
        else
        {
            dropped_.fetch_add(1, std::memory_order_acq_rel);
            block_.assign(blockFrames_ * 2, 0.0f);
        }
        block_.resize(blockFrames_ * 2, 0.0f);
        ready_.insert(ready_.end(), block_.begin(), block_.end());
    }

    for (std::size_t i = 0; i < frames; ++i)
    {
        left[i] = ready_[2 * i];
        right[i] = ready_[2 * i + 1];
    }
    ready_.erase(ready_.begin(), ready_.begin() + static_cast<std::ptrdiff_t>(frames * 2));
}
