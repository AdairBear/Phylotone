#include "phylo/assistant/Assistant.h"

#include "phylo/Scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <sstream>

namespace phylo::assistant
{

namespace
{

const char* const kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

std::string midiName(int midi)
{
    return std::string(kNoteNames[midi % 12]) + std::to_string(midi / 12 - 1);
}

std::string fmt(double v)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

// Object-argument helpers. Each returns false if the field is missing or the wrong type.
bool stringArg(const json::Value& args, const char* key, std::string& out)
{
    const auto* v = args.get(key);
    if (v == nullptr || !v->isString())
        return false;
    out = v->str;
    return true;
}

bool numberArg(const json::Value& args, const char* key, double& out)
{
    const auto* v = args.get(key);
    if (v == nullptr || !v->isNumber())
        return false;
    out = v->number;
    return true;
}

// A change removes material if it drops a pattern, a section, or any notes.
bool removesMaterial(const Scene& before, const Scene& after)
{
    for (const auto& p : before.patterns)
        if (after.findPattern(p.name) == nullptr)
            return true;
    for (const auto& s : before.sections)
    {
        bool found = false;
        for (const auto& t : after.sections)
            found = found || t.name == s.name;
        if (!found)
            return true;
    }
    auto noteCount = [](const Scene& s) {
        std::size_t n = 0;
        for (const auto& p : s.patterns)
            n += p.notes.size();
        return n;
    };
    for (const auto& p : before.patterns)
    {
        const auto* q = after.findPattern(p.name);
        if (q != nullptr && q->notes.size() < p.notes.size())
            return true;
    }
    return noteCount(after) < noteCount(before);
}

std::string describeScene(const Scene& s)
{
    std::ostringstream out;
    out << "Tempo " << fmt(s.tempo) << " BPM, " << s.meter << "/4 meter";
    if (s.hasKey)
        out << ", key " << s.keyRoot << (s.keyMinor ? " minor" : " major");
    out << ".\n";
    if (!s.activePattern.empty())
        out << "Plays pattern: " << s.activePattern << ".\n";
    if (s.patterns.empty())
        out << "No patterns.\n";
    for (const auto& p : s.patterns)
    {
        out << "Pattern " << p.name << ": " << fmt(p.lengthBeats) << " beats, " << p.notes.size() << " notes";
        if (!p.notes.empty())
        {
            int lo = 127, hi = 0;
            for (const auto& n : p.notes)
            {
                lo = std::min(lo, n.note);
                hi = std::max(hi, n.note);
            }
            out << ", range " << midiName(lo) << " to " << midiName(hi);
        }
        out << ".\n";
    }
    for (const auto& s2 : s.sections)
        out << "Section " << s2.name << ": " << s2.bars << " bars, plays " << s2.play.size()
            << " pattern(s). Sections are parsed but not played yet.\n";
    for (const auto& m : s.macros)
        out << "Macro " << m.name << " = " << fmt(m.value) << ".\n";
    return out.str();
}

const char* const kReadSchema = R"({"type":"object","properties":{},"additionalProperties":false})";

} // namespace

const char* modeName(Mode m) noexcept
{
    switch (m)
    {
    case Mode::Off: return "off";
    case Mode::Ask: return "ask";
    case Mode::Assist: return "assist";
    }
    return "ask";
}

bool parseMode(const std::string& s, Mode& out) noexcept
{
    if (s == "off") { out = Mode::Off; return true; }
    if (s == "ask") { out = Mode::Ask; return true; }
    if (s == "assist") { out = Mode::Assist; return true; }
    return false;
}

void ActionLog::append(LogEntry e)
{
    e.seq = nextSeq_++;
    entries_.push_back(std::move(e));
}

bool ActionLog::popLast(LogEntry& out)
{
    if (entries_.empty())
        return false;
    out = std::move(entries_.back());
    entries_.pop_back();
    return true;
}

std::string ActionLog::toJsonLines() const
{
    std::string out;
    for (const auto& e : entries_)
    {
        out += "{\"seq\":" + std::to_string(e.seq);
        out += ",\"tool\":" + json::quote(e.tool);
        out += ",\"args\":" + json::quote(e.argsJson);
        out += ",\"reason\":" + json::quote(e.reason);
        out += ",\"before\":" + json::quote(e.beforeText);
        out += ",\"after\":" + json::quote(e.afterText);
        out += "}\n";
    }
    return out;
}

Assistant::Assistant(ProjectState& p, ActionLog& l) : project(p), log(l) {}

std::vector<ToolSpec> Assistant::tools() const
{
    const std::string reasonProp = R"("reason":{"type":"string","description":"One sentence on why this change, for the log."})";
    return {
        {"read_project", "Read the scene text, the mode, and how many changes are logged.", kReadSchema},
        {"explain_scene", "Describe the scene in plain language: tempo, patterns, ranges, macros.", kReadSchema},
        {"list_instruments", "List loaded and available instruments. None until the plugin host exists.", kReadSchema},
        {"edit_scene",
         "Replace the whole scene text. Must parse. Removing patterns, sections or notes needs approval.",
         R"({"type":"object","properties":{"scene":{"type":"string","description":"The complete new scene text."},)" +
             reasonProp + R"(},"required":["scene","reason"]})"},
        {"set_macro", "Set a macro value from 0 to 1. Adds the macro if it does not exist.",
         R"({"type":"object","properties":{"name":{"type":"string"},"value":{"type":"number","minimum":0,"maximum":1},)" +
             reasonProp + R"(},"required":["name","value","reason"]})"},
        {"undo", "Revert the last logged change.",
         R"({"type":"object","properties":{)" + reasonProp + R"(},"required":["reason"]})"},
    };
}

std::string Assistant::systemPrompt() const
{
    return "You are the assistant inside Phylotone, a music app for coding music. "
           "You work on the current project only. Use the tools to read the scene before you change it. "
           "Explain what you will change, then change it with a clear reason. "
           "Say when you are guessing, and do not present a guess as a fact. "
           "Ask one question at a time when you need a decision. "
           "Treat your suggestions as offers: explain and propose rather than rewrite the piece. "
           "Current mode: " +
           std::string(modeName(project.mode)) + ".";
}

Assistant::Plan Assistant::plan(const ToolCall& call) const
{
    Plan p;
    p.tool = call.name;
    p.argsJson = call.argsJson;

    json::Value args;
    std::string err;
    if (!json::parse(call.argsJson.empty() ? "{}" : call.argsJson, args, err) || !args.isObject())
    {
        p.error = "arguments are not a JSON object";
        return p;
    }

    std::string reason;
    if (!stringArg(args, "reason", reason) || reason.empty())
    {
        p.error = "a non-empty reason is required for changes";
        return p;
    }
    p.reason = reason;

    Plan result;
    if (call.name == "edit_scene")
        result = planEditScene(args, reason);
    else if (call.name == "set_macro")
        result = planSetMacro(args, reason);
    else if (call.name == "undo")
        result = planUndo(reason);
    else
    {
        p.error = "unknown change tool: " + call.name;
        return p;
    }
    result.argsJson = call.argsJson;
    return result;
}

Assistant::Plan Assistant::planEditScene(const json::Value& args, const std::string& reason) const
{
    Plan p;
    p.tool = "edit_scene";
    p.reason = reason;

    std::string scene;
    if (!stringArg(args, "scene", scene))
    {
        p.error = "edit_scene needs a string field 'scene'";
        return p;
    }

    const auto next = parseScene(scene);
    if (!next.ok())
    {
        const auto& e = next.errors.front();
        p.error = "scene does not parse: line " + std::to_string(e.line) + ": " + e.message;
        return p;
    }

    const auto current = parseScene(project.sceneText);
    p.destructive = current.ok() && removesMaterial(current.scene, next.scene);
    p.afterText = scene;
    p.ok = true;
    return p;
}

Assistant::Plan Assistant::planSetMacro(const json::Value& args, const std::string& reason) const
{
    Plan p;
    p.tool = "set_macro";
    p.reason = reason;

    std::string name;
    double value = 0.0;
    if (!stringArg(args, "name", name) || !numberArg(args, "value", value))
    {
        p.error = "set_macro needs 'name' (string) and 'value' (number)";
        return p;
    }
    if (name.empty() || name.find_first_of(" \t\r\n") != std::string::npos)
    {
        p.error = "macro name must be one word";
        return p;
    }
    if (!(value >= 0.0 && value <= 1.0))
    {
        p.error = "macro value must be from 0 to 1";
        return p;
    }

    const std::string line = "macro " + name + " " + fmt(value);
    std::istringstream in(project.sceneText);
    std::string out;
    std::string raw;
    bool replaced = false;
    while (std::getline(in, raw))
    {
        std::istringstream words(raw);
        std::string first, second;
        words >> first >> second;
        if (!replaced && first == "macro" && second == name)
        {
            out += line + "\n";
            replaced = true;
        }
        else
        {
            out += raw + "\n";
        }
    }
    if (!replaced)
        out += line + "\n";

    const auto check = parseScene(out);
    if (!check.ok())
    {
        const auto& e = check.errors.front();
        p.error = "the change would not parse: line " + std::to_string(e.line) + ": " + e.message;
        return p;
    }

    p.afterText = out;
    p.ok = true;
    return p;
}

Assistant::Plan Assistant::planUndo(const std::string& reason) const
{
    Plan p;
    p.tool = "undo";
    p.reason = reason;
    if (log.entries().empty())
    {
        p.error = "nothing to undo";
        return p;
    }
    p.afterText = log.entries().back().beforeText;
    p.ok = true;
    return p;
}

void Assistant::apply(const Plan& p)
{
    if (p.tool == "undo")
    {
        LogEntry undone;
        log.popLast(undone);
        project.sceneText = p.afterText;
        return;
    }

    LogEntry e;
    e.tool = p.tool;
    e.argsJson = p.argsJson;
    e.reason = p.reason;
    e.beforeText = project.sceneText;
    e.afterText = p.afterText;
    project.sceneText = p.afterText;
    log.append(std::move(e));
}

ToolResult Assistant::invoke(const ToolCall& call)
{
    ToolResult r;

    if (call.name == "read_project")
    {
        r.ok = true;
        r.resultJson = "{\"mode\":" + json::quote(modeName(project.mode)) +
                       ",\"scene\":" + json::quote(project.sceneText) +
                       ",\"logged_changes\":" + std::to_string(log.entries().size()) + "}";
        r.message = "Project read.";
        return r;
    }

    if (call.name == "explain_scene")
    {
        const auto parsed = parseScene(project.sceneText);
        if (!parsed.ok())
        {
            r.message = "The scene does not parse, so it cannot be explained.";
            return r;
        }
        r.ok = true;
        r.resultJson = "{\"explanation\":" + json::quote(describeScene(parsed.scene)) + "}";
        r.message = "Scene explained.";
        return r;
    }

    if (call.name == "list_instruments")
    {
        r.ok = true;
        r.resultJson = "{\"instruments\":[],\"note\":\"No plugin host yet (M4).\"}";
        r.message = "No instruments are available yet.";
        return r;
    }

    const bool isChange = call.name == "edit_scene" || call.name == "set_macro" || call.name == "undo";
    if (!isChange)
    {
        r.message = "Unknown tool: " + call.name;
        return r;
    }

    if (project.mode == Mode::Off)
    {
        r.message = "The assistant is off for this project. Nothing was changed.";
        return r;
    }

    const Plan p = plan(call);
    if (!p.ok)
    {
        r.message = p.error;
        return r;
    }

    const bool needsApproval = project.mode == Mode::Ask || (project.mode == Mode::Assist && p.destructive);
    if (needsApproval)
    {
        Proposal prop;
        prop.id = nextProposalId++;
        prop.call = call;
        prop.baseText = project.sceneText;
        proposals_.push_back(prop);
        r.ok = true;
        r.proposed = true;
        r.proposalId = prop.id;
        r.message = std::string("Proposed as change ") + std::to_string(prop.id) +
                    ". It is waiting for the musician's approval and has not been applied.";
        return r;
    }

    apply(p);
    r.ok = true;
    r.applied = true;
    r.message = "Applied: " + p.reason;
    return r;
}

ToolResult Assistant::approve(int proposalId)
{
    ToolResult r;
    auto it = std::find_if(proposals_.begin(), proposals_.end(),
                           [proposalId](const Proposal& p) { return p.id == proposalId; });
    if (it == proposals_.end())
    {
        r.message = "No proposal with that id.";
        return r;
    }

    const Proposal prop = *it;
    proposals_.erase(it);

    // A proposal is only valid against the text it was made for. If the scene has
    // changed since, applying it would overwrite those changes, so ask again instead.
    if (project.sceneText != prop.baseText)
    {
        r.message = "The scene changed after proposal " + std::to_string(prop.id) +
                    " was made, so it was not applied. Propose the change again.";
        return r;
    }

    const Plan p = plan(prop.call);
    if (!p.ok)
    {
        r.message = "The proposal no longer applies: " + p.error;
        return r;
    }
    apply(p);
    r.ok = true;
    r.applied = true;
    r.message = "Applied proposal " + std::to_string(prop.id) + ": " + p.reason;
    return r;
}

bool Assistant::reject(int proposalId)
{
    const auto before = proposals_.size();
    proposals_.erase(std::remove_if(proposals_.begin(), proposals_.end(),
                                    [proposalId](const Proposal& p) { return p.id == proposalId; }),
                     proposals_.end());
    return proposals_.size() != before;
}

Session::Session(Assistant& a, ChatProvider& p, std::string m) : assistant(a), provider(p), model(std::move(m)) {}

void Session::noteAppAction(const std::string& note)
{
    pendingNotes_.push_back(note);
}

TurnResult Session::send(const std::string& userText)
{
    TurnResult out;
    Message user;
    user.role = Role::User;
    user.text = userText;
    if (!pendingNotes_.empty())
    {
        std::string prefix = "[App notes, not written by the user:";
        for (const auto& n : pendingNotes_)
            prefix += "\n- " + n;
        prefix += "]\n\n";
        user.text = prefix + userText;
        pendingNotes_.clear();
    }
    history_.push_back(user);

    for (int round = 0; round <= kMaxToolRounds; ++round)
    {
        ChatRequest req;
        req.systemPrompt = assistant.systemPrompt();
        req.messages = history_;
        req.tools = assistant.tools();
        req.model = model;

        const ChatResponse resp = provider.complete(req);
        if (!resp.ok)
        {
            out.error = resp.error.empty() ? "the provider failed" : resp.error;
            return out;
        }

        Message reply;
        reply.role = Role::Assistant;
        reply.text = resp.text;
        reply.toolCalls = resp.toolCalls;
        history_.push_back(reply);

        if (resp.toolCalls.empty())
        {
            out.ok = true;
            out.text = resp.text;
            return out;
        }

        if (round == kMaxToolRounds)
            break;

        for (const auto& call : resp.toolCalls)
        {
            const ToolResult r = assistant.invoke(call);
            ++out.toolCalls;

            Message result;
            result.role = Role::Tool;
            result.toolCallId = call.id;
            result.text = r.resultJson.empty() ? r.message : r.message + " " + r.resultJson;
            history_.push_back(result);
        }
    }

    out.error = "stopped after " + std::to_string(kMaxToolRounds) + " rounds of tool calls";
    return out;
}

} // namespace phylo::assistant
