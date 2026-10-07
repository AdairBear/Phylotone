#include "MainComponent.h"

namespace
{
constexpr int kMarginPx = 16;
constexpr int kRowHeightPx = 32;
}

MainComponent::MainComponent()
{
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

    statusLabel.setJustificationType(juce::Justification::centredLeft);

    addAndMakeVisible(titleLabel);
    addAndMakeVisible(playButton);
    addAndMakeVisible(stopButton);
    addAndMakeVisible(tempoLabel);
    addAndMakeVisible(tempoSlider);
    addAndMakeVisible(outputLabel);
    addAndMakeVisible(outputBox);
    addAndMakeVisible(refreshButton);
    addAndMakeVisible(statusLabel);

    refreshOutputs();
    setSize(560, 300);
}

MainComponent::~MainComponent() = default;

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

    statusLabel.setBounds(area.removeFromTop(kRowHeightPx));
}
