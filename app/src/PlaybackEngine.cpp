#include "PlaybackEngine.h"

PlaybackEngine::PlaybackEngine() : juce::Thread("Phylotone playback")
{
    sequencer.setSampleRate(kSampleRate);
    sequencer.setTempo(120.0);

    scratch.reserve(64);
    startThread(juce::Thread::Priority::high);
}

PlaybackEngine::~PlaybackEngine()
{
    stopThread(2000);
    std::lock_guard<std::mutex> lock(mutex);
    allNotesOffLocked();
    output.reset();
}

juce::Array<juce::MidiDeviceInfo> PlaybackEngine::availableOutputs()
{
    return juce::MidiOutput::getAvailableDevices();
}

bool PlaybackEngine::openOutput(const juce::String& identifier)
{
    std::lock_guard<std::mutex> lock(mutex);
    allNotesOffLocked();
    output.reset();
    outputName.clear();

    auto device = juce::MidiOutput::openDevice(identifier);
    if (device == nullptr)
        return false;

    output = std::move(device);
    for (const auto& info : juce::MidiOutput::getAvailableDevices())
        if (info.identifier == identifier)
            outputName = info.name;
    return true;
}

juce::String PlaybackEngine::currentOutputName() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return outputName;
}

void PlaybackEngine::play()
{
    std::lock_guard<std::mutex> lock(mutex);
    sequencer.play();
}

void PlaybackEngine::stop()
{
    std::lock_guard<std::mutex> lock(mutex);
    sequencer.stop();
    allNotesOffLocked();
}

void PlaybackEngine::setTempo(double bpm)
{
    std::lock_guard<std::mutex> lock(mutex);
    sequencer.setTempo(bpm);
}

void PlaybackEngine::setMeter(int beatsPerBar)
{
    std::lock_guard<std::mutex> lock(mutex);
    sequencer.setMeter(beatsPerBar);
}

void PlaybackEngine::setPattern(phylo::Pattern p)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (sequencer.isPlaying())
        sequencer.scheduleNextBar(std::move(p));
    else
        sequencer.setPattern(std::move(p));
}

bool PlaybackEngine::isPlaying() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return sequencer.isPlaying();
}

double PlaybackEngine::tempo() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return sequencer.tempo();
}

void PlaybackEngine::allNotesOffLocked()
{
    for (int note = 0; note < 128; ++note)
    {
        if (noteSounding[static_cast<size_t>(note)])
        {
            if (output != nullptr)
                output->sendMessageNow(juce::MidiMessage::noteOff(1, note, (juce::uint8) 0));
            noteSounding[static_cast<size_t>(note)] = false;
        }
    }
}

void PlaybackEngine::run()
{
    while (!threadShouldExit())
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            scratch.clear();
            sequencer.process(kBlockFrames, scratch);

            for (const auto& m : scratch)
            {
                const auto note = static_cast<size_t>(m.data1 & 0x7F);
                const bool isOn = (m.status & 0xF0) == 0x90 && m.data2 > 0;
                noteSounding[note] = isOn;

                if (output != nullptr)
                    output->sendMessageNow(juce::MidiMessage(m.status, m.data1, m.data2));
            }
        }

        // One block of time. sampleOffset within the block is not used for M0.
        wait(1);
    }
}
