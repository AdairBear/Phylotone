// Plays an AudioFeed through the default audio output (M4).
//
// The device calls back on its own thread. The callback only calls AudioFeed::pull,
// which never waits on a plugin: a block not yet rendered plays as silence.
#pragma once

#include "host/AudioFeed.h"

#include <juce_audio_devices/juce_audio_devices.h>

class AudioOutput : private juce::AudioIODeviceCallback
{
public:
    explicit AudioOutput(AudioFeed& feed);
    ~AudioOutput() override;

    // Opens the default stereo output and starts the callback. Returns an error text, or
    // an empty string on success.
    juce::String open();
    void close();

    bool isRunning() const;
    juce::String deviceName() const;

private:
    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData, int numInputChannels,
                                          float* const* outputChannelData, int numOutputChannels,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

    AudioFeed& feed;
    juce::AudioDeviceManager manager;
    bool running = false;
};
