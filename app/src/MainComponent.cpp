#include "MainComponent.h"

namespace
{
constexpr int kMarginPx = 16;
constexpr int kRowHeightPx = 32;
constexpr int kScenePollMs = 250;

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
} // namespace

MainComponent::MainComponent()
{
    sceneFile = sceneLocation();
    if (!sceneFile.existsAsFile())
    {
        sceneFile.getParentDirectory().createDirectory();
        sceneFile.replaceWithText(kDefaultScene);
    }

    titleLabel.setText("Phylotone", juce::dontSendNotification);
    titleLabel.setFont(juce::FontOptions(22.0f, juce::Font::bold));

    playButton.onClick = [this] { engine.play(); updateStatus(); };
    stopButton.onClick = [this] { engine.stop(); updateStatus(); };
    refreshButton.onClick = [this] { refreshOutputs(); };

    tempoLabel.setText("Tempo", juce::dontSendNotification);
    tempoSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    tempoSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 24);
    tempoSlider.setRange(40.0, 240.0, 1.0);
    tempoSlider.setValue(engine.tempo(), juce::dontSendNotification);
    tempoSlider.onValueChange = [this] { engine.setTempo(tempoSlider.getValue()); };

    outputLabel.setText("MIDI out", juce::dontSendNotification);
    outputBox.onChange = [this] { selectOutput(outputBox.getSelectedId()); };

    sceneLabel.setJustificationType(juce::Justification::centredLeft);
    statusLabel.setJustificationType(juce::Justification::centredLeft);

    addAndMakeVisible(titleLabel);
    addAndMakeVisible(playButton);
    addAndMakeVisible(stopButton);
    addAndMakeVisible(tempoLabel);
    addAndMakeVisible(tempoSlider);
    addAndMakeVisible(outputLabel);
    addAndMakeVisible(outputBox);
    addAndMakeVisible(refreshButton);
    addAndMakeVisible(sceneLabel);
    addAndMakeVisible(statusLabel);

    refreshOutputs();
    reloadSceneIfChanged();
    startTimer(kScenePollMs);
    setSize(600, 340);
}

MainComponent::~MainComponent()
{
    stopTimer();
}

void MainComponent::timerCallback()
{
    reloadSceneIfChanged();
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
    tempoSlider.setValue(next.tempo, juce::dontSendNotification);

    // Only send the pattern when its content changed, so an unrelated edit
    // does not restart the loop.
    const auto* newActive = next.activePattern.empty() ? nullptr : next.findPattern(next.activePattern);
    const auto* oldActive = scene.activePattern.empty() ? nullptr : scene.findPattern(scene.activePattern);
    const bool changed = newActive != nullptr &&
                         (oldActive == nullptr || !phylo::samePattern(*newActive, *oldActive));
    if (changed)
        engine.setPattern(phylo::buildPattern(*newActive));

    scene = next;
    juce::String message = "Scene: " + sceneFile.getFileName() + " loaded";
    if (next.activePattern.empty())
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
}
