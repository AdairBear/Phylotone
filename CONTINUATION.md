# Phylotone: continuation brief

*For any session picking up this project. Last updated 2026-10-08.*

## Read first
1. `docs/CONTRACT.md`: the completion contract. Everything is measured against it.
2. `docs/MILESTONES.md`: the path, with acceptance checks per milestone.
3. `docs/ASSISTANT.md`: the assistant agent spec. It is a required part of the
   product, not an add-on.

## Where we are
- Contract drafted (v0.1). Milestones drafted. Assistant spec drafted.
- M0 in progress (branch `m0-skeleton`):
  - Done: JUCE-free core sequencer (`core/`): Pattern, Sequencer, integer-frame
    timing. 14 core tests in `tests/core/`, passing under g++ and under
    ASan/UBSan. Build with `cmake -S . -B build && cmake --build build && ctest --test-dir build`.
  - Done (branch `m0-app-shell`): JUCE 8.0.9 Standalone app (`app/`) with
    play, stop, tempo, MIDI output picker, and a looping C major arpeggio.
    Build: `cmake -S . -B build -DPHYLO_BUILD_APP=ON`. Verified to build and
    launch under Xvfb on Linux.
  - Done: CI workflow (`.github/workflows/ci.yml`) for Ubuntu, macOS and
    Windows: builds core and app, runs core tests.
  - Not verified yet: audible MIDI output. This sandbox has no ALSA sequencer,
    so no MIDI device was available. The send path has not run here, and the
    CI runs have not been observed.
  - Known limit: M0 timing uses a 1 ms sleep loop, so jitter is a few ms or
    worse (Windows default timer resolution is ~15 ms). The audio-driven clock
    is M1 work.
- M1 in progress (branch `m1-scene`):
  - Done: scene language first cut (`core/include/phylo/Scene.h`): tempo, meter,
    key (stored, not used yet), patterns with note names, sections (parsed and
    validated, not played), `play` for the active pattern. Parse errors carry
    line numbers and every error is reported.
  - Done: pattern changes scheduled to the next bar (`Sequencer::scheduleNextBar`).
    The block is split at the bar line, so the new pattern starts exactly on the bar.
  - Done: the app watches `Documents/Phylotone/scene.phy` and reloads it on change.
    A change to the pattern lands on the next bar while playing.
  - Core tests: 34, passing under g++ and ASan/UBSan. Four regression tests
    fail against the pre-review sequencer, so they catch the reviewed bugs.
  - Not verified: the audible M1 check. The sandbox has no MIDI device.
  - Decision made without you: scene syntax is our own (see Scene.h header), not
    Strata-like. CONTRACT.md lists this as open; change it if you want Strata.
  - Review (independent, Opus subagent) found and fixed: notes left sounding at a
    switch; a duplicate bar-line event at fractional bar lengths; a `tempo nan`
    that hung the app; NaN and odd numbers accepted by the parser; a tempo change
    that moved the playhead; a missing or empty scene file resetting the tempo.
    Time is now kept in beats, so tempo changes keep the playhead and bar lines.
  - Known limits: sections are not played (M6). Key is not used (M3). Bar timing
    is exact in the core but MIDI send timing is still the 1 ms loop. The
    pattern swap frees memory on the playback thread (fine for the 1 ms loop;
    must move to the audio thread's deferred free before the audio callback).
    Immediate pattern changes while stopped start at the playhead, not at a bar.
- M2 merged (PR #4). Still open from M2: the accept check needs a person; live
  chat is untested; scene file is written per turn, not per change; approve and
  reject are not reported to the model; slow local models can hit the 60 s timeout.
- M3 in progress (branch `m3-generator`):
  - Done: scene directives `chords` (one chord per bar, 1 to 16), `seed`, and
    `generate pad bass`. Generator in `core/include/phylo/Generator.h`, with
    macros density (bass), tension (pad sevenths), space (pad length and level).
  - Determinism: the random source is hand-written integer code, not a standard
    library distribution, so output is the same across compilers. Every random
    draw is made whether used or not, so changing density does not reshuffle
    other rolls.
  - The app plays the generated take in place of the `play` pattern, and a
    macro or seed change lands on the next bar.
  - Core tests: 62, passing under g++ and ASan/UBSan. Accept check
    (same seed and settings give identical MIDI twice) is a core test.
  - Not verified: audible output (no MIDI device here). Voicings are simple
    (root, third, fifth, optional seventh; bass root plus rolls). Not reviewed
    musically yet.
- Open decisions are listed at the end of CONTRACT.md.

## Next
1. Review and merge PR for M4 headless render (offline take through Akazi XL), on Thomas's go-ahead.
2. Thomas to listen to the take (WAV from the scratch driver) and say whether it sounds right.
   This is the M4 ear check; the numbers only show sound is produced.
3. M4 app side: route PlaybackEngine's blocks through `PipelinedRenderer` and an audio output.
   Needs a machine with audio out.
4. M4 accept: play Akazi XL from the generator; kill the host, app keeps running.
5. M5 app side (levels, pan, master bus, record in the UI): needs the same audio output.
6. M2 is closed in code. Live checks need a person.

## Working rules
- New work goes on a branch with a pull request. Thomas reviews and merges.
- Each milestone ends with a hardening pass and a note in the session log below.
- Do not change the contract without recording why in the session log.
- Akazi XL is a separate repo and a plugin target. Its clean-room and trademark
  rules are in its own CLAUDE.md; do not copy its code into this project.

## Session log
- 2026-10-07: Contract, milestones and assistant spec drafted in `docs/`.
- 2026-10-07: Working name set to Phylotone as a placeholder. Naming to be revisited in a dedicated session.
- 2026-10-07: M0 core sequencer built with tests. Found and fixed a bug: when the
  loop length in frames is not an integer, events at a loop boundary could be
  dropped depending on block size. Fixed by checking one loop either side of each
  block. Block-size independence is now tested for sizes 1 to 96000.
- 2026-10-07: M0 core merged (PR #1). M0 app shell and CI on `m0-app-shell`.
  Known limits are listed under "Where we are".
- 2026-10-07: M1 first cut on `m1-scene`: scene parser, next-bar pattern changes, scene file watch.
- 2026-10-08: M2 core on `m2-assistant`. Provider-neutral chat interface
  (`core/include/phylo/assistant/ChatProvider.h`). Provider adapters for Claude,
  OpenAI, Gemini and OpenAI-compatible local servers in `app/providers`, verified
  against handwritten wire-format JSON only. Assistant core (tools, Off/Ask/Assist
  modes, action log with undo, stale-proposal guard, turn loop) with 51 core
  tests. Decision: the model is a user choice across vendors, not a fixed one.
  Keys come from environment variables, never from the project folder.
- 2026-10-08: M2 app panel on `m2-assistant`. Provider choice (Claude, OpenAI,
  Gemini, Local), model name, base URL, and mode (Off, Ask, Assist; default Ask).
  Settings file holds no keys. Requests run on a background thread, one at a time.
  Linux builds use libcurl, since JUCE's own Linux networking is HTTP only.
  Known gaps: the scene file is written once per turn, not after each change;
  approval and rejection are not reported back to the model; a slow local model
  can hit the 60 s timeout.
  Not verified: a live chat with any provider. No keys or network here.
  Accept check (M2) still needs a person: change the bass rhythm, hear it land on
  the next bar, then undo it.
- 2026-10-08: PR #3 (M1 scene) merged. PR #4 (M2 assistant) merged on Thomas's go-ahead.
- 2026-10-08: PR #5 (M3) merged. M3 hardening: a self-review of the generator found no defects beyond the tests; no separate hardening PR.
- 2026-10-08: M4 slice 1 on `m4-plugin-host`: host wire protocol (`core/include/phylo/host/Wire.h`), JUCE-free, with 7 tests and a hostile-length guard. Next: the plugin-host process (JUCE plugin loading, out of process), the app-side supervisor, and the Akazi XL load.
- 2026-10-08: PR #6 (M4 slice 1, wire protocol) merged. M4 slice 2 on `m4-plugin-host-2`: `phylo-plughost` executable (one plugin per process, JUCE plugin formats, speaks the wire protocol on stdin/stdout). Checked over pipes: load failure reported, silent output without a plugin, damaged stream exits 1. NOT yet checked: loading a real plugin (none on this machine). Next: the app-side supervisor (spawn, timeout, restart, silence on crash) and the kill test.
- 2026-10-08: PR #7 (plugin host) merged. M4 slice 3 on `m4-supervisor`: app-side `PluginHostSupervisor` and `ProcessPipe` (POSIX and Windows pipes, since JUCE's ChildProcess cannot write stdin). Kill test `phylo_supervisor_check` passes: a killed host gives silence, a cooldown, then a restart that renders again. Found and fixed: the host read stdin with fread, which waits for a full buffer on a live pipe. NOT yet wired into PlaybackEngine. Renders are synchronous with a 250 ms reply timeout, so this is not safe for live playback yet. Windows build unverified until CI.
- 2026-10-08: PR #8 (supervisor) merged. M4 slice 4 on `m4-playback-host`: the real Akazi XL VST3 (built from its repo with JUCE 8.0.15 into a scratch dir, not in its repo) now loads in `phylo-plughost` and processes blocks. Needed: `JUCE_PLUGINHOST_VST3=1` (default off in JUCE 8.0.9) and a message manager on the host's main thread. The host exits via `_Exit` to skip JUCE's shutdown leak check. NOT yet proven: sound. Akazi XL is a sampler and outputs silence until a sample is loaded. Loading a sample means setting plugin state: its `sourcePath` property loads a WAV. The host protocol has no state message yet. Next: add SetState to the wire protocol and check that a rendered note is audible.
- 2026-10-08: Plugin hosting decided: separate process per plugin (recorded in CONTRACT.md section 7). M4 starts on `m4-plugin-host`.
- 2026-10-08: M3 generator on `m3-generator`: chords, seed and generate directives;
  pads and bass; macros density, tension, space. Seeded integer RNG for
  cross-platform identical output. Decision: the generated take replaces the
  `play` pattern when a `generate` line is present.
- 2026-10-08: M4 slice 5 on `m4-state`: GetState/State and SetState/StateResult on the host wire protocol (raw plugin state bytes; `Host::getState`/`setState` in `phylo-plughost`). Core test for the new frames and the refused unknown types. Verified: Akazi XL loaded in the host, state read (882 bytes), `sourcePath` pointed at a 440 Hz WAV inside the processor payload, then a held note gave RMS 0.23 and peak 0.32 with silence before and after. The driver is a scratch script outside the repo. Not verified: generator-to-plugin playback; the supervisor is still not wired into PlaybackEngine.
- 2026-10-08: PR #10 (M4 slice 5, plugin state) merged on Thomas's go-ahead.
- 2026-10-08: M4 slice 6 on `m4-playback`: `PipelinedRenderer` (app/src/host). Runs the
  supervisor on a worker thread. `submit()` and `take()` never wait; a full queue (4)
  drops the block and counts it. Check `phylo_renderer_check` (ctest
  `phylo_renderer_pipelined`) passes 30 of 30 runs with a dead host, and under
  ThreadSanitizer. Not verified: audio output or the Akazi XL render through it
  (PlaybackEngine not changed yet).
- 2026-10-08: PR #11 (M4 slice 6, pipelined renderer) merged on Thomas's go-ahead.
- 2026-10-08: M2 hardening on `m2-hardening`, part 1: approve and reject are now sent to the
  model. `Session::noteAppAction` queues a note, which goes ahead of the next user message,
  marked as not written by the user. Covered by core test `session_sends_app_actions_with_the_next_user_message`.
  Still open from M2: the scene file is written per turn, not per change (needs the write
  moved off the background thread, since it calls `say()`); a slow local model can hit the
  60 s timeout (needs a per-provider timeout). Not verified: a live model reading the note.
- 2026-10-08: PR #12 (M2 hardening, approve/reject notes) merged on Thomas's go-ahead.
- 2026-10-08: M2 hardening part 2 on `m2-timeout`: the request timeout depends on the provider.
  `requestTimeoutMs`: 60 s for hosted providers, 5 minutes for Local. Checked in `phylo_app_checks`.
  Still open from M2: the scene file is written per turn, not per change (needs the write moved
  off the background thread, since it calls `say()`). Not verified: a slow local model actually
  finishing inside 5 minutes.
- 2026-10-08: PR #13 (M2 timeout) merged on Thomas's go-ahead.
- 2026-10-08: M2 per-change writes on `m2-per-change-write`: `Assistant::onSceneChanged` fires after
  each applied edit and each undo. The controller writes the scene file at that point through
  `persistScene`, which does no UI work, so it is safe on the background thread. A conflict or a
  failed write is recorded and reported on the message thread by `reportWriteOutcome` when the
  turn ends. Core test `assistant_reports_each_scene_change` passes (72 core checks, 0 failed).
  Not verified: the controller path, since it needs the UI. A live turn that edits the scene,
  and a change to the file during a turn, should be tried by hand.
- M2 is now closed in the code. Remaining checks need a person: the M2 accept check and a live chat.
- 2026-10-08: M5 slice 1 on `m5-mixer`: `Mixer` (per-track gain, equal-power pan, mute, master gain; no limiter) in `core/include/phylo/Mixer.h`, and float WAV write/read plus `TakeRecorder` in `core/include/phylo/Wav.h`. Seven new core tests; `phylo_core_tests` reports 79 tests, 0 failed checks, ctest 5/5. Accept check passes at core level: a recorded take, read back and rendered again, matches bit for bit. Not verified: the app wiring (levels, pan and record in the UI) and any audio output.
- 2026-10-09: PR #15 (M5 slice 1) merged on Thomas's go-ahead. M5 slice 2 on `m5-app-mixer`: core test `mixer_output_does_not_depend_on_block_size` (same input mixed in 512- and 128-frame blocks gives identical samples). 80 core checks, 0 failed; ctest 5/5. The app side of M5 has no audio output to attach to yet, so it waits for M4 playback wiring.
- 2026-10-09: M4 headless render on `m4-headless-render`. `PluginHostSupervisor::setState` (restores plugin state, 5 s timeout, a refusal keeps the host up). `renderPatternOffline` (app/src/host/OfflineTake): runs the sequencer block by block, sends the MIDI to a renderer callback, and returns the audio and note-on frames. Check `phylo_offline_take_check` (ctest `phylo_offline_take`): note-ons at 0/24000/48000/72000, identical at 48- and 512-frame blocks, silence in, silence out. Real run (scratch driver outside the repo, Akazi XL VST3 in a Debug host): state set, a 4-beat arpeggio rendered as 8 s; note-ons at the right frames; RMS about 0.21, peak 0.32, host Running with no error. NOT verified: how it sounds (WAV sent for an ear check), the Release host, and the playback path (PlaybackEngine not changed).
