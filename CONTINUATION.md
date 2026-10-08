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
1. Thomas reviews and edits CONTRACT.md.
2. Settle the open decisions that block M0 (name, plugin hosting approach).
3. Finish M0: app shell with play/stop and MIDI out, then CI on all three platforms.
4. M0 hardening pass, then a note here.

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
- 2026-10-08: M3 generator on `m3-generator`: chords, seed and generate directives;
  pads and bass; macros density, tension, space. Seeded integer RNG for
  cross-platform identical output. Decision: the generated take replaces the
  `play` pattern when a `generate` line is present.
