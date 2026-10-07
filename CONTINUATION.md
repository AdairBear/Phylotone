# Phylotone: continuation brief

*For any session picking up this project. Last updated 2026-10-07.*

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
  - Core tests: 28, passing under g++ and ASan/UBSan.
  - Not verified: the audible M1 check. The sandbox has no MIDI device.
  - Decision made without you: scene syntax is our own (see Scene.h header), not
    Strata-like. CONTRACT.md lists this as open; change it if you want Strata.
  - Known limits: sections are not played (M6). Key is not used (M3). Bar timing
    is exact in the core but MIDI send timing is still the 1 ms loop.
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
