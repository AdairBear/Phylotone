// Owns the sequencer and the MIDI output, and runs playback on its own thread.
//
// Threading: the UI thread sends commands (play, stop, tempo, open output) under
// a mutex. The playback thread renders the sequencer in small blocks and sends
// the resulting MIDI. Both sides only touch the sequencer and the output while
// holding the mutex.
//
// Pattern changes are scheduled on bar lines (see phylo/Sequencer.h).
// M0 timing is driven by a 1 ms sleep loop, so it is accurate to a few
// milliseconds at best. The audio-driven clock comes in M1.

#pragma once

#include "phylo/Sequencer.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <memory>
#include <mutex>

class PlaybackEngine : private juce::Thread
{
public:
    PlaybackEngine();
    ~PlaybackEngine() override;

    // Lists MIDI output devices as (name, identifier) pairs.
    static juce::Array<juce::MidiDeviceInfo> availableOutputs();

    // Opens a MIDI output by identifier. Closes any previous output first.
    // Returns false if the device could not be opened.
    bool openOutput(const juce::String& identifier);

    juce::String currentOutputName() const;

    void play();
    void stop();
    void setTempo(double bpm);
    void setMeter(int beatsPerBar);

    // Sets the track's pattern. While playing, the change lands on the next bar.
    // While stopped, it takes effect immediately.
    void setPattern(phylo::Pattern p);
    bool isPlaying() const;
    double tempo() const;

private:
    void run() override;

    // Sends note-offs for every note that is still on. Called after stop and
    // on output change, so no note is left hanging. Caller holds the mutex.
    void allNotesOffLocked();

    static constexpr int kSampleRate = 48000;
    static constexpr int kBlockFrames = 48; // 1 ms at 48 kHz

    mutable std::mutex mutex;
    phylo::Sequencer sequencer;
    std::unique_ptr<juce::MidiOutput> output;
    juce::String outputName;
    std::array<bool, 128> noteSounding {};
    std::vector<phylo::MidiOut> scratch;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PlaybackEngine)
};
