#pragma once

#include "AssistantController.h"
#include "AssistantPanel.h"
#include "PlaybackEngine.h"

#include "phylo/Scene.h"

#include <optional>

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
    std::optional<phylo::ScenePattern> sentTake; // the last pattern sent to the engine

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

    // Declared after the scene members and before the panel: the panel holds a
    // reference to the controller, so it must be destroyed first.
    AssistantController assistantController;
    AssistantPanel assistantPanel { assistantController };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
