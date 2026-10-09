#include "AssistantController.h"

#include "phylo/assistant/Json.h"

namespace
{
juce::String utf8(const std::string& s)
{
    return juce::String::fromUTF8(s.data(), static_cast<int>(s.size()));
}

bool isBlank(const juce::String& s)
{
    return s.trim().isEmpty();
}

juce::String summarise(const phylo::assistant::Proposal& p)
{
    juce::String text = "#" + juce::String(p.id) + "  " + utf8(p.call.name);
    phylo::json::Value args;
    std::string error;
    if (phylo::json::parse(p.call.argsJson, args, error))
    {
        if (const auto* reason = args.get("reason"); reason != nullptr && reason->isString())
            text << ": " << utf8(reason->str);
    }
    return text;
}
} // namespace

AssistantController::AssistantController(juce::File scene, juce::File settingsPath)
    : sceneFile(std::move(scene)), settingsFile(std::move(settingsPath))
{
    if (settingsFile.existsAsFile())
        settings_ = parseSettings(settingsFile.loadFileAsString().toStdString());

    project.mode = settings_.mode;
    syncFromFile();
}

AssistantController::~AssistantController()
{
    *alive = false;
    // Abort the request in flight, then wait for the turn to unwind. Every
    // later request in that turn fails at once, so this does not wait long.
    cancel->cancel();
    pool.removeAllJobs(true, -1);
}

void AssistantController::say(const juce::String& who, const juce::String& text)
{
    if (onTranscript)
        onTranscript(who, text);
}

void AssistantController::notifyChanged()
{
    if (onStateChanged)
        onStateChanged();
}

//==============================================================================
// Settings

void AssistantController::saveSettings()
{
    settingsFile.getParentDirectory().createDirectory();
    if (!settingsFile.replaceWithText(utf8(serialiseSettings(settings_)), false, false, nullptr))
        say("app", "Could not save " + settingsFile.getFullPathName());
}

void AssistantController::setProvider(ProviderKind p)
{
    if (busy || p == settings_.provider)
        return;
    settings_.provider = p;
    saveSettings();
    resetSession("provider changed");
    notifyChanged();
}

void AssistantController::setModel(const juce::String& model)
{
    const auto m = model.trim().toStdString();
    if (busy || m == settings_.model())
        return;
    settings_.model() = m;
    saveSettings();
    resetSession("model changed");
    notifyChanged();
}

void AssistantController::setLocalBaseUrl(const juce::String& url)
{
    auto u = url.trim().toStdString();
    if (u.empty())
        u = AssistantSettings::kDefaultLocalBaseUrl;
    if (busy || u == settings_.localBaseUrl)
        return;
    settings_.localBaseUrl = u;
    saveSettings();
    if (settings_.provider == ProviderKind::Local)
        resetSession("server URL changed");
    notifyChanged();
}

void AssistantController::setMode(phylo::assistant::Mode m)
{
    if (busy || m == settings_.mode)
        return;
    settings_.mode = m;
    project.mode = m;
    saveSettings();
    say("app", "Mode: " + juce::String(phylo::assistant::modeName(m)));
    notifyChanged();
}

//==============================================================================
// Provider and session

void AssistantController::resetSession(const juce::String& why)
{
    jassert(!busy);
    const bool hadSession = session != nullptr && !session->history().empty();
    session.reset();
    provider.reset();
    if (hadSession)
        say("app", "New conversation (" + why + ").");
}

bool AssistantController::ensureSession()
{
    if (session != nullptr)
        return true;

    if (settings_.model().empty())
    {
        say("app", "Enter a model name for " + juce::String(providerDisplayName(settings_.provider)) +
                       " first.");
        return false;
    }

    auto post = makeJuceHttpPost(cancel, kHttpTimeoutMs);
    using namespace phylo::providers;
    switch (settings_.provider)
    {
    case ProviderKind::Anthropic: provider = std::make_unique<AnthropicProvider>(post); break;
    case ProviderKind::OpenAI: provider = std::make_unique<OpenAIProvider>(post); break;
    case ProviderKind::Gemini: provider = std::make_unique<GeminiProvider>(post); break;
    case ProviderKind::Local:
        provider = std::make_unique<OpenAICompatibleProvider>(post, settings_.localBaseUrl);
        break;
    }

    session = std::make_unique<phylo::assistant::Session>(assistant, *provider, settings_.model());
    return true;
}

//==============================================================================
// Scene file

juce::String AssistantController::readSceneFile() const
{
    return sceneFile.existsAsFile() ? sceneFile.loadFileAsString() : juce::String();
}

bool AssistantController::syncFromFile()
{
    if (!sceneFile.existsAsFile())
        return false;
    sceneModified = sceneFile.getLastModificationTime();
    const auto text = readSceneFile();
    if (isBlank(text))
        return false; // likely a save in progress; keep the current text
    project.sceneText = text.toStdString();
    return true;
}

void AssistantController::pollSceneFile()
{
    if (busy || !sceneFile.existsAsFile())
        return;
    if (sceneFile.getLastModificationTime() != sceneModified)
        syncFromFile();
}

void AssistantController::writeIfChanged(const std::string& textBefore)
{
    if (project.sceneText == textBefore)
        return;

    const auto onDisk = readSceneFile().toStdString();
    if (onDisk != textBefore && !isBlank(utf8(onDisk)))
    {
        say("app", "The scene file changed on disk while the assistant was working, so its change "
                   "was not written. Reloaded the file; ask again if you still want the change.");
        project.sceneText = onDisk;
        sceneModified = sceneFile.getLastModificationTime();
        return;
    }

    sceneFile.getParentDirectory().createDirectory();
    // nullptr line endings: write the text exactly as the core produced it.
    if (!sceneFile.replaceWithText(utf8(project.sceneText), false, false, nullptr))
    {
        say("app", "Could not write " + sceneFile.getFullPathName());
        return;
    }
    sceneModified = sceneFile.getLastModificationTime();
}

//==============================================================================
// Turns

bool AssistantController::send(const juce::String& text)
{
    if (busy || isBlank(text))
        return false;

    syncFromFile();
    project.mode = settings_.mode;
    if (!ensureSession())
        return false;

    say("you", text.trim());

    busy = true;
    notifyChanged();

    std::string textBefore = project.sceneText;
    auto* s = session.get();
    const auto userText = text.trim().toStdString();
    auto flag = alive;

    pool.addJob([this, s, userText, flag, textBefore = std::move(textBefore)]() mutable
    {
        // Background thread: owns project, log, assistant and session until finishTurn.
        auto result = s->send(userText);
        juce::MessageManager::callAsync(
            [this, flag, result = std::move(result), textBefore = std::move(textBefore)]() mutable
            {
                if (*flag)
                    finishTurn(result, std::move(textBefore));
            });
    });
    return true;
}

void AssistantController::finishTurn(const phylo::assistant::TurnResult& result, std::string textBefore)
{
    busy = false;

    writeIfChanged(textBefore);

    if (!result.ok)
        say("app", "Error: " + utf8(result.error));
    else if (!result.text.empty())
        say(utf8(provider != nullptr ? provider->name() : std::string("assistant")), utf8(result.text));

    if (result.toolCalls > 0)
        say("app", juce::String(result.toolCalls) + " tool call" + (result.toolCalls == 1 ? "" : "s") +
                       " this turn.");

    refreshProposals();
    notifyChanged();
}

void AssistantController::refreshProposals()
{
    proposalSnapshot.clear();
    for (const auto& p : assistant.proposals())
        proposalSnapshot.push_back({ p.id, summarise(p) });
}

std::string AssistantController::describeProposal(int proposalId) const
{
    for (const auto& p : assistant.proposals())
        if (p.id == proposalId)
            return juce::String(summarise(p)).toStdString();
    return "(no such proposal)";
}

void AssistantController::approve(int proposalId)
{
    if (busy)
        return;
    syncFromFile();
    const std::string what = describeProposal(proposalId);
    const auto textBefore = project.sceneText;
    const auto r = assistant.approve(proposalId);
    writeIfChanged(textBefore);
    const std::string outcome = r.message.empty() ? std::string(r.ok ? "Approved." : "Could not approve.") : r.message;
    say("app", utf8(outcome));
    if (session != nullptr)
        session->noteAppAction("The user approved proposal " + std::to_string(proposalId) + ": " + what +
                               " Result: " + outcome);
    refreshProposals();
    notifyChanged();
}

void AssistantController::reject(int proposalId)
{
    if (busy)
        return;
    const std::string what = describeProposal(proposalId);
    const bool rejected = assistant.reject(proposalId);
    say("app", rejected ? "Rejected proposal " + juce::String(proposalId) + "."
                        : "No proposal " + juce::String(proposalId) + ".");
    if (rejected && session != nullptr)
        session->noteAppAction("The user rejected proposal " + std::to_string(proposalId) + ": " + what);
    refreshProposals();
    notifyChanged();
}
