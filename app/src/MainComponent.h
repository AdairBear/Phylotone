#pragma once

#include "PlaybackEngine.h"

#include "phylo/Scene.h"

#include <juce_gui_basics/juce_gui_basics.h>

// Main window. Watches the scene file and reloads it when it changes.
class MainComponent : public juce::Component, private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;

    void refreshOutputs();
    void selectOutput(int comboId);
    void updateStatus();

    // Reads the scene file if it changed. On a parse error, keeps the current
    // scene and reports the first error.
    void reloadSceneIfChanged();
    void applyScene(const phylo::Scene& scene);

    PlaybackEngine engine;
    juce::Array<juce::MidiDeviceInfo> outputs;

    juce::File sceneFile;
    juce::Time sceneModified;
    bool sceneLoaded = false;
    phylo::Scene scene;
    std::string activePatternName;

    juce::Label titleLabel;
    juce::TextButton playButton { "Play" };
    juce::TextButton stopButton { "Stop" };
    juce::Label tempoLabel;
    juce::Slider tempoSlider;
    juce::Label outputLabel;
    juce::ComboBox outputBox;
    juce::TextButton refreshButton { "Refresh" };
    juce::Label sceneLabel;
    juce::Label statusLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
