// Turns a playing pattern into a steady stream of stereo audio for an audio device (M4).
//
// pull() is called from the audio thread. It never waits: blocks are queued on the
// sink, and whatever the sink has finished is played; until then the output is silence.
// Each generated block gives exactly one block of output, so the stream never runs out
// or skips, even when the renderer is late or drops a block (that block is silence).
// The cost is latency: output lags the sequencer by the sink's queue depth.
//
// Control is lock-free. The message thread sets the tempo, meter, sample rate, transport
// and pattern through atomics (last write wins); the audio thread reads them at the start
// of each pull and applies any change to its own sequencer. The feed itself never takes a
// lock on the audio thread. Still on that path: the sink's submit and take (PipelinedRenderer
// holds its own mutex briefly), and the buffers the feed grows. See the notes in AudioFeed.cpp.
#pragma once

#include "AudioBlockSink.h"
#include "phylo/Sequencer.h"

#include <atomic>
#include <cstddef>
#include <deque>
#include <vector>

class AudioFeed
{
public:
    AudioFeed(AudioBlockSink& sink, double sampleRate, std::size_t blockFrames = 512);
    ~AudioFeed();

    AudioFeed(const AudioFeed&) = delete;
    AudioFeed& operator=(const AudioFeed&) = delete;

    // Message thread only. Each call replaces the previous value; the audio thread applies
    // the latest one on its next pull. The sample rate is set when the audio device starts.
    void setSampleRate(double sampleRate);
    void setTempo(double bpm);
    void setMeter(int beatsPerBar);
    // While playing, the new pattern lands on the next bar, as on the MIDI path. A second
    // call before the next pull replaces the first.
    void setPattern(phylo::Pattern pattern);
    void play();
    void stop();

    // Audio thread only. Writes `frames` stereo frames, split into left and right. Never waits.
    void pull(float* left, float* right, std::size_t frames);

    // Blocks the sink refused. Each one became silence. Safe from any thread.
    std::size_t droppedBlocks() const;

private:
    void applyControls(); // audio thread: moves the atomics into the sequencer

    AudioBlockSink& sink_;
    std::size_t blockFrames_;

    // Written by the message thread, read by the audio thread.
    std::atomic<double> sampleRate_;
    std::atomic<double> tempo_;
    std::atomic<int> meter_;
    std::atomic<bool> wantPlaying_{false};
    std::atomic<phylo::Pattern*> pendingPattern_{nullptr}; // owned by whoever holds the pointer
    std::atomic<std::size_t> dropped_{0};
    std::atomic<std::size_t> controlSerial_{0}; // bumped by each tempo, meter or sample-rate write

    // Audio thread only.
    phylo::Sequencer seq_;
    std::size_t appliedSerial_ = static_cast<std::size_t>(-1);
    std::deque<float> ready_; // interleaved stereo, ready to play
    std::vector<phylo::MidiOut> midi_;
    std::vector<float> block_;
};
