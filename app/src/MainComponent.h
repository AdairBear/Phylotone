#pragma once

#include "AssistantController.h"
#include "AssistantPanel.h"
#include "PlaybackEngine.h"
#include "PlaybackSettings.h"

#include "phylo/Scene.h"

#include <memory>
#include <optional>

#include <juce_gui_basics/juce_gui_basics.h>

class AudioFeed;
class AudioOutput;
class PipelinedRenderer;

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

    // Plugin audio (M4): the generator's notes rendered through Akazi XL and played on
    // the default audio output, alongside the MIDI output.
    void setPluginAudio(bool on);
    void pushToFeed();
    void loadPlaybackSettings();

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

    // Plugin audio. Built the first time the toggle is turned on. Declared in
    // dependency order, so the output is destroyed before the feed and the feed
    // before the renderer.
    PlaybackSettings playbackSettings;
    std::unique_ptr<PipelinedRenderer> renderer;
    std::unique_ptr<AudioFeed> feed;
    std::unique_ptr<AudioOutput> audio;
    bool playing = false;
    juce::String pluginError; // empty when plugin audio is fine or off

    juce::Label titleLabel;
    juce::TextButton playButton { "Play" };
    juce::TextButton stopButton { "Stop" };
    juce::ToggleButton pluginAudioToggle { "Plugin audio" };
    juce::Label tempoLabel;
    juce::Slider tempoSlider;
    juce::Label outputLabel;
    juce::ComboBox outputBox;
    juce::TextButton refreshButton { "Refresh" };
    juce::Label sceneLabel;
    juce::Label statusLabel;
    juce::Label pluginLabel;

    // Declared after the scene members and before the panel: the panel holds a
    // reference to the controller, so it must be destroyed first.
    AssistantController assistantController;
    AssistantPanel assistantPanel { assistantController };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
