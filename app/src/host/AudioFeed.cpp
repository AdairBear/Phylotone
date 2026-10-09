#include "AudioFeed.h"
#include "OfflineTake.h"

AudioFeed::AudioFeed(AudioBlockSink& sink, double sampleRate, std::size_t blockFrames)
    : sink_(sink), blockFrames_(blockFrames > 0 ? blockFrames : 1)
{
    seq_.setSampleRate(sampleRate);
}

void AudioFeed::setSampleRate(double sampleRate)
{
    std::lock_guard<std::mutex> lock(mutex_);
    seq_.setSampleRate(sampleRate);
}

void AudioFeed::setTempo(double bpm)
{
    std::lock_guard<std::mutex> lock(mutex_);
    seq_.setTempo(bpm);
}

void AudioFeed::setMeter(int beatsPerBar)
{
    std::lock_guard<std::mutex> lock(mutex_);
    seq_.setMeter(beatsPerBar);
}

void AudioFeed::setPattern(phylo::Pattern pattern)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (seq_.isPlaying())
        seq_.scheduleNextBar(std::move(pattern));
    else
        seq_.setPattern(std::move(pattern));
}

void AudioFeed::play()
{
    std::lock_guard<std::mutex> lock(mutex_);
    seq_.play();
}

void AudioFeed::stop()
{
    std::lock_guard<std::mutex> lock(mutex_);
    seq_.stop();
}

std::size_t AudioFeed::droppedBlocks() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
}

void AudioFeed::pull(float* left, float* right, std::size_t frames)
{
    std::lock_guard<std::mutex> lock(mutex_);

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
            ++dropped_;
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
