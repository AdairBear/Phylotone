#include "AssistantPanel.h"

namespace
{
constexpr int kRowPx = 28;
constexpr int kGapPx = 8;
constexpr int kLabelColPx = 64;
constexpr int kProposalRowPx = 32;
constexpr int kMaxVisibleProposals = 3;

// ComboBox ids are 1-based.
int providerId(ProviderKind p) { return static_cast<int>(p) + 1; }
int modeId(phylo::assistant::Mode m) { return static_cast<int>(m) + 1; }
} // namespace

//==============================================================================

class AssistantPanel::ProposalRow : public juce::Component
{
public:
    using Decide = std::function<void(int id, bool approve)>;

    ProposalRow(Decide decide, const AssistantController::ProposalInfo& info, bool enabled)
    {
        label.setText(info.summary, juce::dontSendNotification);
        label.setTooltip(info.summary);
        label.setMinimumHorizontalScale(0.8f);
        approve.onClick = [decide, id = info.id] { decide(id, true); };
        reject.onClick = [decide, id = info.id] { decide(id, false); };
        approve.setEnabled(enabled);
        reject.setEnabled(enabled);
        addAndMakeVisible(label);
        addAndMakeVisible(approve);
        addAndMakeVisible(reject);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(0, 2);
        reject.setBounds(r.removeFromRight(72));
        r.removeFromRight(6);
        approve.setBounds(r.removeFromRight(80));
        r.removeFromRight(6);
        label.setBounds(r);
    }

private:
    juce::Label label;
    juce::TextButton approve { "Approve" };
    juce::TextButton reject { "Reject" };
};

void AssistantPanel::ProposalList::resized()
{
    auto r = getLocalBounds();
    for (auto* row : rows)
        row->setBounds(r.removeFromTop(kProposalRowPx));
}

//==============================================================================

AssistantPanel::AssistantPanel(AssistantController& c) : controller(c)
{
    titleLabel.setText("Assistant", juce::dontSendNotification);
    titleLabel.setFont(juce::FontOptions(17.0f, juce::Font::bold));

    for (int i = 0; i < kNumProviderKinds; ++i)
        providerBox.addItem(providerDisplayName(static_cast<ProviderKind>(i)), i + 1);
    providerBox.onChange = [this]
    {
        commitModel();
        controller.setProvider(static_cast<ProviderKind>(providerBox.getSelectedId() - 1));
    };

    using phylo::assistant::Mode;
    modeBox.addItem("Mode: Off", modeId(Mode::Off));
    modeBox.addItem("Mode: Ask", modeId(Mode::Ask));
    modeBox.addItem("Mode: Assist", modeId(Mode::Assist));
    modeBox.setTooltip("Off: read only. Ask: every change is a proposal. "
                       "Assist: low-risk changes apply, destructive ones are proposals.");
    modeBox.onChange = [this] { controller.setMode(static_cast<Mode>(modeBox.getSelectedId() - 1)); };

    modelLabel.setText("Model", juce::dontSendNotification);
    modelEditor.setSelectAllWhenFocused(true);
    modelEditor.onReturnKey = [this] { commitModel(); };
    modelEditor.onFocusLost = [this] { commitModel(); };

    urlLabel.setText("Server", juce::dontSendNotification);
    urlEditor.setTextToShowWhenEmpty(AssistantSettings::kDefaultLocalBaseUrl,
                                     urlEditor.findColour(juce::TextEditor::textColourId).withAlpha(0.5f));
    auto commitUrl = [this] { controller.setLocalBaseUrl(urlEditor.getText()); };
    urlEditor.onReturnKey = commitUrl;
    urlEditor.onFocusLost = commitUrl;

    hintLabel.setFont(juce::FontOptions(13.0f));
    hintLabel.setMinimumHorizontalScale(0.7f);

    transcript.setMultiLine(true, true);
    transcript.setReadOnly(true);
    transcript.setCaretVisible(false);
    transcript.setScrollbarsShown(true);

    proposalsView.setViewedComponent(&proposalList, false);
    proposalsView.setScrollBarsShown(true, false);

    input.setTextToShowWhenEmpty("Ask the assistant...",
                                 input.findColour(juce::TextEditor::textColourId).withAlpha(0.5f));
    input.onReturnKey = [this] { sendInput(); };
    sendButton.onClick = [this] { sendInput(); };

    statusLabel.setFont(juce::FontOptions(13.0f));

    for (auto* comp : std::initializer_list<juce::Component*> {
             &titleLabel, &providerBox, &modeBox, &modelLabel, &modelEditor, &urlLabel, &urlEditor,
             &hintLabel, &transcript, &proposalsLabel, &proposalsView, &input, &sendButton, &statusLabel })
        addAndMakeVisible(comp);

    controller.onTranscript = [this](const juce::String& who, const juce::String& text)
    { appendTranscript(who, text); };
    controller.onStateChanged = [this] { syncFromController(); };

    appendTranscript("app", "Keys are read from the environment (ANTHROPIC_API_KEY, OPENAI_API_KEY, "
                            "GEMINI_API_KEY) and never stored.");
    syncFromController();
}

AssistantPanel::~AssistantPanel()
{
    controller.onTranscript = nullptr;
    controller.onStateChanged = nullptr;
}

juce::String AssistantPanel::modelHint() const
{
    switch (controller.settings().provider)
    {
    case ProviderKind::Anthropic:
        return "Model id, e.g. " + juce::String(AssistantSettings::kDefaultAnthropicModel) +
               ". Key from ANTHROPIC_API_KEY.";
    case ProviderKind::OpenAI: return "Model id as OpenAI names it. Key from OPENAI_API_KEY.";
    case ProviderKind::Gemini: return "Model id as Google names it. Key from GEMINI_API_KEY.";
    case ProviderKind::Local: return "Model name your server lists (e.g. ollama list). No key needed.";
    }
    return {};
}

void AssistantPanel::commitModel()
{
    controller.setModel(modelEditor.getText());
}

void AssistantPanel::sendInput()
{
    if (controller.isBusy())
        return;
    commitModel();
    if (controller.send(input.getText()))
        input.clear();
}

void AssistantPanel::appendTranscript(const juce::String& who, const juce::String& text)
{
    transcript.moveCaretToEnd();
    if (!transcript.isEmpty())
        transcript.insertTextAtCaret("\n");
    transcript.insertTextAtCaret(who + ": " + text + "\n");
    transcript.moveCaretToEnd();
}

void AssistantPanel::syncFromController()
{
    const auto& s = controller.settings();
    const bool busy = controller.isBusy();

    providerBox.setSelectedId(providerId(s.provider), juce::dontSendNotification);
    modeBox.setSelectedId(modeId(s.mode), juce::dontSendNotification);

    if (!modelEditor.hasKeyboardFocus(true))
        modelEditor.setText(juce::String::fromUTF8(s.model().c_str()), juce::dontSendNotification);
    modelEditor.setTextToShowWhenEmpty(s.provider == ProviderKind::Anthropic ? "" : "required",
                                       modelEditor.findColour(juce::TextEditor::textColourId).withAlpha(0.5f));
    if (!urlEditor.hasKeyboardFocus(true))
        urlEditor.setText(juce::String::fromUTF8(s.localBaseUrl.c_str()), juce::dontSendNotification);
    hintLabel.setText(modelHint(), juce::dontSendNotification);

    const bool local = s.provider == ProviderKind::Local;
    urlLabel.setVisible(local);
    urlEditor.setVisible(local);

    for (auto* comp : std::initializer_list<juce::Component*> { &providerBox, &modeBox, &modelEditor,
                                                                &urlEditor, &sendButton })
        comp->setEnabled(!busy);

    statusLabel.setText(busy ? "Waiting for " + juce::String(providerDisplayName(s.provider)) + "..."
                             : juce::String(),
                        juce::dontSendNotification);

    rebuildProposals();
    resized();
}

void AssistantPanel::rebuildProposals()
{
    const auto& list = controller.proposals();
    const bool enabled = !controller.isBusy();

    // Approve and Reject rebuild this list, which deletes the clicked button, so
    // the decision runs after the click handler has returned.
    auto decide = [safe = juce::Component::SafePointer<AssistantPanel>(this)](int id, bool approve)
    {
        juce::MessageManager::callAsync([safe, id, approve]
        {
            if (safe == nullptr)
                return;
            if (approve)
                safe->controller.approve(id);
            else
                safe->controller.reject(id);
        });
    };

    proposalList.rows.clear();
    for (const auto& p : list)
        proposalList.addAndMakeVisible(proposalList.rows.add(new ProposalRow(decide, p, enabled)));

    proposalsLabel.setText(list.empty() ? "Proposals: none" : "Proposals (" + juce::String((int) list.size()) + ")",
                           juce::dontSendNotification);
}

void AssistantPanel::resized()
{
    auto area = getLocalBounds();

    titleLabel.setBounds(area.removeFromTop(kRowPx));
    area.removeFromTop(kGapPx / 2);

    auto pickRow = area.removeFromTop(kRowPx);
    modeBox.setBounds(pickRow.removeFromRight(130));
    pickRow.removeFromRight(kGapPx);
    providerBox.setBounds(pickRow);
    area.removeFromTop(kGapPx);

    auto modelRow = area.removeFromTop(kRowPx);
    modelLabel.setBounds(modelRow.removeFromLeft(kLabelColPx));
    modelEditor.setBounds(modelRow);

    if (urlEditor.isVisible())
    {
        area.removeFromTop(kGapPx / 2);
        auto urlRow = area.removeFromTop(kRowPx);
        urlLabel.setBounds(urlRow.removeFromLeft(kLabelColPx));
        urlEditor.setBounds(urlRow);
    }

    auto hintRow = area.removeFromTop(20);
    hintRow.removeFromLeft(kLabelColPx);
    hintLabel.setBounds(hintRow);
    area.removeFromTop(kGapPx);

    // Bottom up: status, input row, proposals.
    statusLabel.setBounds(area.removeFromBottom(20));
    auto inputRow = area.removeFromBottom(kRowPx);
    sendButton.setBounds(inputRow.removeFromRight(80));
    inputRow.removeFromRight(kGapPx);
    input.setBounds(inputRow);
    area.removeFromBottom(kGapPx);

    const int rows = proposalList.rows.size();
    const int visible = juce::jmin(rows, kMaxVisibleProposals);
    proposalsView.setBounds(area.removeFromBottom(visible * kProposalRowPx));
    proposalsLabel.setBounds(area.removeFromBottom(22));
    area.removeFromBottom(kGapPx / 2);

    const int listWidth = proposalsView.getWidth() -
                          (rows > kMaxVisibleProposals ? proposalsView.getScrollBarThickness() : 0);
    proposalList.setSize(juce::jmax(0, listWidth), rows * kProposalRowPx);

    transcript.setBounds(area);
}
