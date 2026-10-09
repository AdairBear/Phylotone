#include "AudioOutput.h"

AudioOutput::AudioOutput(AudioFeed& f) : feed(f) {}

AudioOutput::~AudioOutput()
{
    close();
}

juce::String AudioOutput::open()
{
    if (running)
        return {};

    // No inputs, two outputs, default devices.
    const auto error = manager.initialiseWithDefaultDevices(0, 2);
    if (error.isNotEmpty())
        return error;

    manager.addAudioCallback(this);
    running = true;
    return {};
}

void AudioOutput::close()
{
    if (!running)
        return;

    manager.removeAudioCallback(this);
    running = false;
}

bool AudioOutput::isRunning() const
{
    return running;
}

juce::String AudioOutput::deviceName() const
{
    if (auto* device = manager.getCurrentAudioDevice())
        return device->getName();
    return {};
}

void AudioOutput::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    if (device != nullptr)
        feed.setSampleRate(device->getCurrentSampleRate());
}

void AudioOutput::audioDeviceStopped()
{
}

void AudioOutput::audioDeviceIOCallbackWithContext(const float* const*, int, float* const* outputChannelData,
                                                   int numOutputChannels, int numSamples,
                                                   const juce::AudioIODeviceCallbackContext&)
{
    float* left = numOutputChannels > 0 ? outputChannelData[0] : nullptr;
    float* right = numOutputChannels > 1 ? outputChannelData[1] : left;
    if (left == nullptr)
        return;

    feed.pull(left, right, static_cast<std::size_t>(numSamples));
}
