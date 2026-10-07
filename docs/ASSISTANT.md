# Phylotone: assistant agent spec

*Draft v0.1. The assistant is part of the product, not a feature bolted onto it.*

## Purpose

A capable collaborator inside every project. It helps you compose, explain what
the music is doing, change things safely, and keep track of decisions. It is
useful whether or not the music is playing.

## Where it lives

- **In the app:** a panel next to the scene editor.
- **From the project folder:** a command that reads the project and answers
  questions, for working away from the app.
- **Headless:** the same tools, callable from scripts or a chat session.

All three share one project state and one action log.

## What it can see

- Scene files and the arrangement.
- Macro values and their history.
- Takes, with their seeds and settings.
- Its own notes file for the project.
- Live state (playing, tempo, track levels), when audio is running.

It cannot read files outside the project folder.

## What it can do (named tools)

| Tool | Scope | Needs confirmation |
|------|-------|--------------------|
| `read_project` | Read scenes, arrangement, macros, takes | No |
| `explain_scene` | Describe what a scene does in plain language | No |
| `edit_scene` | Change a scene file | Only if it removes material |
| `set_macro` | Change a macro value | No |
| `generate_pattern` | Produce a pattern into a named slot | No |
| `render_preview` | Offline render to a preview file | No |
| `start_recording` / `stop_recording` | Control recording | Starting: no. Overwriting a take: yes |
| `list_instruments` | Show loaded and available plugins | No |
| `load_plugin` | Load a plugin not yet used in this project | Yes |
| `change_master` | Change the master bus | Yes |
| `delete_material` | Remove a pattern, section or take | Yes |
| `undo` | Revert the last logged action | No |

## Rules

1. **Confirm the destructive.** Anything in the "Yes" column waits for a clear
   yes. A vague reply is not a yes.
2. **Log everything.** Each action records what changed, why the assistant chose
   it, and the previous value. The log is in the project folder.
3. **Undo is always there.** Any logged action can be reverted, in order.
4. **Stay in scope.** It works on the current project only.
5. **Say what it doesn't know.** It flags guesses ("I think this will sound
   thinner, I haven't rendered it") and does not present them as facts.
6. **Ask one question at a time.** If it needs a decision, it asks the one that
   matters most.
7. **Respect the musician.** Its suggestions are offers. The default is to explain
   and propose, not to rewrite the piece.

## Modes

- **Ask:** proposes changes and waits for approval. The starting mode.
- **Assist:** applies low-risk changes (macro values, previews, patterns in new
   slots) and asks for everything else.
- **Off:** read-only. It can explain but not change anything.

The mode is per project and can be changed at any time.

## Memory

A `ASSISTANT_NOTES.md` in the project folder holds things the musician has said
they like or don't like, decisions made, and open questions. It is plain text,
editable by hand, and the assistant reads it at the start of every session.
Nothing goes in it that the musician has not said or agreed to.

## Open questions

- Which model powers it, and whether it runs locally, through the API, or both.
- Whether the in-app panel can stream its replies while the audio engine runs.
- How much of the project to send when the assistant runs remotely.
