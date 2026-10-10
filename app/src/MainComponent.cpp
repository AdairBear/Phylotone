#include "MainComponent.h"

#include "AudioOutput.h"
#include "PlaybackSettings.h"
#include "host/AudioFeed.h"
#include "host/PipelinedRenderer.h"

#include "phylo/Generator.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <vector>

namespace
{
constexpr int kMarginPx = 16;
constexpr int kRowHeightPx = 32;
constexpr int kScenePollMs = 250;
constexpr double kFeedSampleRateHz = 48000.0; // replaced by the device rate when it starts
constexpr std::size_t kFeedBlockFrames = 512;

const char* kPlaybackTemplate =
    "# Plugin audio settings. Fill in both lines, then turn on Plugin audio in the app.\n"
    "# Any line left empty is read from PHYLO_PLUGHOST and PHYLO_PLUGIN instead.\n"
    "# plughost=/path/to/phylo-plughost\n"
    "# plugin=/path/to/Akazi XL.vst3\n"
    "# state=/path/to/plugin-state.bin  (optional: raw plugin state, restored when Plugin audio starts)\n";

const char* kDefaultScene =
    "// Phylotone scene. Save this file while the app runs and the change\n"
    "// lands on the next bar.\n"
    "tempo 120\n"
    "meter 4\n"
    "play arp\n"
    "\n"
    "pattern arp 4\n"
    "  0 C4 100 0.9\n"
    "  1 E4 100 0.9\n"
    "  2 G4 100 0.9\n"
    "  3 C5 100 0.9\n";

juce::File sceneLocation()
{
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
        .getChildFile("Phylotone")
        .getChildFile("scene.phy");
}

juce::File assistantSettingsLocation()
{
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
        .getChildFile("Phylotone")
        .getChildFile("assistant.settings");
}

juce::File playbackSettingsLocation()
{
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
        .getChildFile("Phylotone")
        .getChildFile("playback.txt");
}
} // namespace

MainComponent::MainComponent()
    : assistantController(sceneLocation(), assistantSettingsLocation())
{
    sceneFile = sceneLocation();
    if (!sceneFile.existsAsFile())
    {
        sceneFile.getParentDirectory().createDirectory();
        sceneFile.replaceWithText(kDefaultScene);
    }

    titleLabel.setText("Phylotone", juce::dontSendNotification);
    titleLabel.setFont(juce::FontOptions(22.0f, juce::Font::bold));

    playButton.onClick = [this]
    {
        engine.play();
        playing = true;
        if (feed)
            feed->play();
        updateStatus();
    };
    stopButton.onClick = [this]
    {
        engine.stop();
        playing = false;
        if (feed)
            feed->stop();
        updateStatus();
    };
    pluginAudioToggle.onClick = [this] { setPluginAudio(pluginAudioToggle.getToggleState()); };
    refreshButton.onClick = [this] { refreshOutputs(); };

    tempoLabel.setText("Tempo", juce::dontSendNotification);
    tempoSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    tempoSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 24);
    tempoSlider.setRange(40.0, 240.0, 1.0);
    tempoSlider.setValue(engine.tempo(), juce::dontSendNotification);
    tempoSlider.onValueChange = [this]
    {
        engine.setTempo(tempoSlider.getValue());
        if (feed)
            feed->setTempo(tempoSlider.getValue());
    };

    outputLabel.setText("MIDI out", juce::dontSendNotification);
    outputBox.onChange = [this] { selectOutput(outputBox.getSelectedId()); };

    sceneLabel.setJustificationType(juce::Justification::centredLeft);
    statusLabel.setJustificationType(juce::Justification::centredLeft);
    pluginLabel.setJustificationType(juce::Justification::centredLeft);

    addAndMakeVisible(titleLabel);
    addAndMakeVisible(playButton);
    addAndMakeVisible(stopButton);
    addAndMakeVisible(pluginAudioToggle);
    addAndMakeVisible(tempoLabel);
    addAndMakeVisible(tempoSlider);
    addAndMakeVisible(outputLabel);
    addAndMakeVisible(outputBox);
    addAndMakeVisible(refreshButton);
    addAndMakeVisible(sceneLabel);
    addAndMakeVisible(statusLabel);
    addAndMakeVisible(pluginLabel);
    addAndMakeVisible(assistantPanel);

    loadPlaybackSettings();
    refreshOutputs();
    reloadSceneIfChanged();
    startTimer(kScenePollMs);
    setSize(600, 780);
}

MainComponent::~MainComponent()
{
    stopTimer();
    // Close the device before the feed and the renderer it reads from are destroyed.
    if (audio)
        audio->close();
}

void MainComponent::loadPlaybackSettings()
{
    const auto file = playbackSettingsLocation();
    PlaybackSettings fromFile;
    if (file.existsAsFile())
    {
        fromFile = parsePlaybackSettings(file.loadFileAsString().toStdString());
    }
    else
    {
        file.getParentDirectory().createDirectory();
        file.replaceWithText(kPlaybackTemplate);
    }

    playbackSettings = resolvePlaybackSettings(fromFile, std::getenv("PHYLO_PLUGHOST"), std::getenv("PHYLO_PLUGIN"));
}

void MainComponent::setPluginAudio(bool on)
{
    pluginError = {};

    if (!on)
    {
        if (audio)
            audio->close();
        if (feed)
            feed->stop();
        pluginAudioToggle.setToggleState(false, juce::dontSendNotification);
        updateStatus();
        return;
    }

    if (!playbackSettings.complete())
        pluginError = "Set plughost and plugin in Documents/Phylotone/playback.txt";
    else if (!juce::File(playbackSettings.pluginHost).existsAsFile())
        pluginError = "Plugin host not found: " + juce::String(playbackSettings.pluginHost);
    else if (!juce::File(playbackSettings.plugin).exists())
        pluginError = "Plugin not found: " + juce::String(playbackSettings.plugin);

    if (pluginError.isNotEmpty())
    {
        pluginAudioToggle.setToggleState(false, juce::dontSendNotification);
        updateStatus();
        return;
    }

    // Built on first use, so nothing runs (no host, no device) until it is asked for.
    if (!renderer)
    {
        renderer = std::make_unique<PipelinedRenderer>(
            std::make_unique<PluginHostSupervisor>(playbackSettings.pluginHost, playbackSettings.plugin));
        feed = std::make_unique<AudioFeed>(*renderer, kFeedSampleRateHz, kFeedBlockFrames);
        audio = std::make_unique<AudioOutput>(*feed);
        pushToFeed();

        // The plugin has no sample until its state is restored. Restore it before the first block.
        if (!playbackSettings.state.empty())
        {
            juce::MemoryBlock bytes;
            if (juce::File(juce::String(playbackSettings.state)).loadFileAsData(bytes))
                renderer->restoreState(std::vector<std::uint8_t>(static_cast<const std::uint8_t*>(bytes.getData()),
                                                                 static_cast<const std::uint8_t*>(bytes.getData()) +
                                                                     bytes.getSize()));
            else
                pluginError = "State file not found: " + juce::String(playbackSettings.state);
        }
    }

    const auto error = audio->open();
    if (error.isNotEmpty())
    {
        pluginError = "Audio device: " + error;
        pluginAudioToggle.setToggleState(false, juce::dontSendNotification);
        updateStatus();
        return;
    }

    // If the transport is already playing, start the plugin audio from the top of the pattern.
    if (playing)
        feed->play();
    pluginAudioToggle.setToggleState(true, juce::dontSendNotification);
    updateStatus();
}

void MainComponent::pushToFeed()
{
    if (!feed)
        return;
    feed->setTempo(scene.tempo);
    feed->setMeter(scene.meter);
    if (sentTake.has_value())
        feed->setPattern(phylo::buildPattern(*sentTake));
}

void MainComponent::timerCallback()
{
    reloadSceneIfChanged();
    assistantController.pollSceneFile();
}

void MainComponent::reloadSceneIfChanged()
{
    // A missing file (some editors save by delete and rename) keeps the current scene.
    if (!sceneFile.existsAsFile())
    {
        sceneLabel.setText("Scene file missing; keeping the current scene", juce::dontSendNotification);
        return;
    }

    const auto modified = sceneFile.getLastModificationTime();
    if (sceneLoaded && modified == sceneModified)
        return;

    sceneModified = modified;
    sceneLoaded = true;

    const auto text = sceneFile.loadFileAsString().toStdString();
    if (text.find_first_not_of(" \t\r\n") == std::string::npos)
    {
        // An empty file is most likely a save in progress. Keep the current scene.
        sceneLabel.setText("Scene file is empty; keeping the current scene", juce::dontSendNotification);
        return;
    }

    const auto result = phylo::parseScene(text);

    if (!result.ok())
    {
        const auto& e = result.errors.front();
        sceneLabel.setText("Scene error, line " + juce::String(e.line) + ": " + e.message +
                               " (keeping the current scene)",
                           juce::dontSendNotification);
        return;
    }

    applyScene(result.scene);
}

void MainComponent::applyScene(const phylo::Scene& next)
{
    // Tempo and meter take effect at once; they do not wait for a bar.
    engine.setTempo(next.tempo);
    engine.setMeter(next.meter);
    if (feed)
    {
        feed->setTempo(next.tempo);
        feed->setMeter(next.meter);
    }
    tempoSlider.setValue(next.tempo, juce::dontSendNotification);

    // The track is either a generated take or the play-line pattern. Only send it
    // when its content changed, so an unrelated edit does not restart the loop.
    std::optional<phylo::ScenePattern> take;
    if (phylo::hasGeneratedTake(next))
        take = phylo::generateTake(next);
    else if (!next.activePattern.empty())
        if (const auto* p = next.findPattern(next.activePattern))
            take = *p;

    const bool changed = take.has_value() && (!sentTake.has_value() || !phylo::samePattern(*take, *sentTake));
    if (changed)
    {
        engine.setPattern(phylo::buildPattern(*take));
        if (feed)
            feed->setPattern(phylo::buildPattern(*take));
        sentTake = take;
    }

    scene = next;
    juce::String message = "Scene: " + sceneFile.getFileName() + " loaded";
    if (phylo::hasGeneratedTake(next))
        message += changed ? " (generated take on next bar)" : " (take unchanged)";
    else if (next.activePattern.empty())
        message += " (no play line, pattern unchanged)";
    else if (changed)
        message += " (pattern " + juce::String(next.activePattern) + " on next bar)";
    sceneLabel.setText(message, juce::dontSendNotification);
    updateStatus();
}

void MainComponent::refreshOutputs()
{
    outputs = PlaybackEngine::availableOutputs();
    outputBox.clear(juce::dontSendNotification);

    for (int i = 0; i < outputs.size(); ++i)
        outputBox.addItem(outputs[i].name, i + 1);

    if (outputs.isEmpty())
    {
        outputBox.setTextWhenNothingSelected("No MIDI outputs found");
        outputBox.setEnabled(false);
    }
    else
    {
        outputBox.setEnabled(true);
        // Keep the current device if it is still present, else take the first.
        const auto current = engine.currentOutputName();
        int selected = 1;
        for (int i = 0; i < outputs.size(); ++i)
            if (outputs[i].name == current)
                selected = i + 1;
        outputBox.setSelectedId(selected, juce::dontSendNotification);
        selectOutput(selected);
    }
    updateStatus();
}

void MainComponent::selectOutput(int comboId)
{
    const int index = comboId - 1;
    if (index < 0 || index >= outputs.size())
        return;

    if (!engine.openOutput(outputs[index].identifier))
        statusLabel.setText("Could not open " + outputs[index].name, juce::dontSendNotification);
    updateStatus();
}

void MainComponent::updateStatus()
{
    const auto name = engine.currentOutputName();
    const juce::String transport = engine.isPlaying() ? "Playing" : "Stopped";
    statusLabel.setText(transport + "  |  " + (name.isEmpty() ? "no MIDI output" : "out: " + name),
                        juce::dontSendNotification);

    juce::String pluginText = (audio && audio->isRunning()) ? "Plugin audio on: " + audio->deviceName()
                                                            : juce::String("Plugin audio off");
    if (audio && audio->isRunning() && feed && feed->droppedBlocks() > 0)
        pluginText += "  |  dropped blocks: " + juce::String(static_cast<int>(feed->droppedBlocks()));
    if (renderer)
    {
        const auto state = renderer->stateStatus();
        if (!state.empty())
            pluginText += "  |  " + juce::String(state);
    }
    if (pluginError.isNotEmpty())
        pluginText += "  |  " + pluginError;
    pluginLabel.setText(pluginText, juce::dontSendNotification);
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff1b1d21));
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced(kMarginPx);

    titleLabel.setBounds(area.removeFromTop(kRowHeightPx + 8));
    area.removeFromTop(kMarginPx / 2);

    auto transportRow = area.removeFromTop(kRowHeightPx);
    playButton.setBounds(transportRow.removeFromLeft(96));
    transportRow.removeFromLeft(8);
    stopButton.setBounds(transportRow.removeFromLeft(96));
    transportRow.removeFromLeft(16);
    pluginAudioToggle.setBounds(transportRow.removeFromLeft(160));
    area.removeFromTop(kMarginPx / 2);

    auto tempoRow = area.removeFromTop(kRowHeightPx);
    tempoLabel.setBounds(tempoRow.removeFromLeft(80));
    tempoSlider.setBounds(tempoRow);
    area.removeFromTop(kMarginPx / 2);

    auto outputRow = area.removeFromTop(kRowHeightPx);
    outputLabel.setBounds(outputRow.removeFromLeft(80));
    refreshButton.setBounds(outputRow.removeFromRight(96));
    outputRow.removeFromRight(8);
    outputBox.setBounds(outputRow);
    area.removeFromTop(kMarginPx);

    sceneLabel.setBounds(area.removeFromTop(kRowHeightPx));
    statusLabel.setBounds(area.removeFromTop(kRowHeightPx));
    pluginLabel.setBounds(area.removeFromTop(kRowHeightPx));
    area.removeFromTop(kMarginPx);

    assistantPanel.setBounds(area);
}
