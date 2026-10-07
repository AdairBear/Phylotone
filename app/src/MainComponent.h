#pragma once

#include "PlaybackEngine.h"

#include <juce_gui_basics/juce_gui_basics.h>

class MainComponent : public juce::Component
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void refreshOutputs();
    void selectOutput(int comboId);
    void updateStatus();

    PlaybackEngine engine;
    juce::Array<juce::MidiDeviceInfo> outputs;

    juce::TextButton playButton { "Play" };
    juce::TextButton stopButton { "Stop" };
    juce::Slider tempoSlider;
    juce::Label tempoLabel;
    juce::ComboBox outputBox;
    juce::Label outputLabel;
    juce::Label statusLabel;

    juce::Label titleLabel;
    juce::TextButton refreshButton { "Refresh" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
