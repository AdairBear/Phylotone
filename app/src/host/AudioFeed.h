// Turns a playing pattern into a steady stream of stereo audio for an audio device (M4).
//
// pull() is called from the audio thread. It never waits: blocks are queued on the
// sink, and whatever the sink has finished is played; until then the output is silence.
// Each generated block gives exactly one block of output, so the stream never runs out
// or skips, even when the renderer is late or drops a block (that block is silence).
// The cost is latency: output lags the sequencer by the sink's queue depth.
#pragma once

#include "AudioBlockSink.h"
#include "phylo/Sequencer.h"

#include <cstddef>
#include <deque>
#include <mutex>
#include <vector>

class AudioFeed
{
public:
    AudioFeed(AudioBlockSink& sink, double sampleRate, std::size_t blockFrames = 512);

    // Setters are called from the message thread. They take effect on the next block.
    // The sample rate is set when the audio device starts, which is when it is known.
    void setSampleRate(double sampleRate);
    void setTempo(double bpm);
    void setMeter(int beatsPerBar);
    // While playing, the new pattern lands on the next bar, as on the MIDI path.
    void setPattern(phylo::Pattern pattern);
    void play();
    void stop();

    // Writes `frames` stereo frames, split into left and right. Never waits.
    void pull(float* left, float* right, std::size_t frames);

    // Blocks the sink refused. Each one became silence.
    std::size_t droppedBlocks() const;

private:
    AudioBlockSink& sink_;
    std::size_t blockFrames_;

    mutable std::mutex mutex_; // guards the sequencer and the counters
    phylo::Sequencer seq_;
    std::deque<float> ready_; // interleaved stereo, ready to play
    std::vector<phylo::MidiOut> midi_;
    std::vector<float> block_;
    std::size_t dropped_ = 0;
};
