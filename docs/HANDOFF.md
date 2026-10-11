# Phylotone: handoff

*Status as of 2026-10-10. Repo: github.com/AdairBear/Phylotone. Base branch: `kickoff/contract` at `5f5fe5b`.*

This is the current handoff. It supersedes the "Next" list in `CONTINUATION.md`, which is kept for the session log. Read `docs/CONTRACT.md` as the reference for everything below. This file does not restate it.

---

## 1. Where things stand

- **All pull requests are merged**, #1 to #25. The only open change is the docs branch that adds this file and refreshes `CONTINUATION.md` (see section 9).
- **Last merged:** #25, the sample interface proposal (`docs/SAMPLE_INTERFACE.md`, docs only).
- **Milestones:** M0, M1, M2 and M3 are in the code. M4 is partly built. M5 has a core slice. M6, M7 and M8 are not started.
- **Nothing has been heard.** No audio device, MIDI device, or network was available to the session that built this. Every audible check is still open.

## 2. Working rules

- New work goes on a branch from the base, with a pull request. Thomas reviews and merges.
- Merge only on his explicit "Merge" or "Merge and continue".
- Each milestone ends with a hardening pass and a note in the session log in `CONTINUATION.md`.
- Do not change the contract without recording why in the session log.
- Akazi XL is a separate repo and a plugin target. Do not copy its code into Phylotone. Its own `CLAUDE.md` sets the clean-room and trademark rules (see section 7).
- Commit messages and PR bodies end with the attribution lines the session was given.

## 3. Components

### Core (`core/`, JUCE-free C++17, library `phylo_core`)

| Header | What it does |
|---|---|
| `Sequencer.h` | Integer-frame timing. Pattern changes land on the next bar (`scheduleNextBar`). Time is kept in beats, so tempo changes keep the playhead. |
| `Scene.h` | Scene language first cut: tempo, meter, key (stored, not yet used), patterns with note names, sections (parsed and validated, not played), `chords`, `seed`, `generate pad bass`. Parse errors carry line numbers. |
| `Generator.h` | Pads and bass following scene chords. Macros: density (bass), tension (pad sevenths), space (pad length and level). Seeded, with hand-written integer RNG so output is the same on every compiler. |
| `Mixer.h` | Per-track gain, equal-power pan, mute, master gain. No limiter. |
| `Wav.h` | Float WAV write and read, `TakeRecorder`. |
| `assistant/` | Provider-neutral chat interface (`ChatProvider.h`), tools, Off/Ask/Assist modes, action log with undo, stale-proposal guard, turn loop. |
| `host/Wire.h` | Plugin host wire protocol (see section 4). |

### App (`app/`, JUCE 8.0.9, target `PhylotoneApp`)

- `MainComponent`: transport, MIDI output picker, scene file watch, the Plugin audio toggle, status line.
- `PlaybackEngine`: the MIDI path. Unchanged by M4.
- `AssistantController`, `AssistantPanel`, `AssistantSettings`: the assistant. Settings file holds no keys. Keys come from environment variables.
- Providers in `app/providers`: Claude, OpenAI, Gemini, and OpenAI-compatible local servers. Verified only against handwritten wire-format JSON. No live chat has run.
- `AudioOutput`: opens the default stereo output and calls `AudioFeed::pull` from the device callback.
- `PlaybackSettings`: reads `Documents/Phylotone/playback.txt` (keys `plughost`, `plugin`, `state`). Empty values fall back to env vars `PHYLO_PLUGHOST` and `PHYLO_PLUGIN`. The file wins. The file is created with commented instructions on first launch.
- `host/`:
  - `AudioFeed`: sequencer blocks of 512 frames. Setters are atomics with last-write-wins. Pattern handoff is an atomic pointer.
  - `AudioBlockSink`: submit and take, never waits.
  - `PipelinedRenderer`: runs the supervisor on a worker thread. Two SPSC rings of 4 with no mutex. A full ring means the block is dropped and counted.
  - `PluginHostSupervisor`: spawns `phylo-plughost`. Start timeout 5 s, reply timeout 250 ms for audio, state timeout 5 s, restart cooldown 1 s. Keeps the restored state bytes and re-sends them after every host start.
  - `ProcessPipe`: POSIX and Windows pipes.
  - `OfflineTake`: `buildBlock` and `renderPatternOffline`. The offline render and the live feed share `buildBlock`.

### Plugin host (`phylo-plughost`, `app/plughost/PluginHostMain.cpp`)

- One plugin per process. A crashing plugin cannot take the app down.
- Loads plugin formats through JUCE (`JUCE_PLUGINHOST_VST3=1`, needed on JUCE 8.0.9).
- Speaks length-prefixed frames on stdin and stdout.
- Exits with `_Exit` to skip JUCE's shutdown leak check.

## 4. Wire protocol

Frame types: Hello=1, Load=2, Loaded=3, Process=4, Audio=5, Shutdown=6, Error=7, GetState=8, State=9, SetState=10, StateResult=11. Unknown types are refused. The receiver has a hostile-length guard.

## 5. Build and test

```sh
# Core only
cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure

# App (needs JUCE, fetched by CMake at configure time)
cmake -S . -B build -DPHYLO_BUILD_APP=ON
```

Current ctest: 8 of 8 passing. Check targets (plain C++ except where noted):

- `phylo_core_tests`: core suite. 80 checks at last count, 0 failed.
- `phylo_supervisor_check` (`phylo_supervisor_kill`): kill test, state refused with no plugin, state re-sent after restart.
- `phylo_renderer_check` (`phylo_renderer_pipelined`): renderer never blocks its caller with a dead host. 20 of 20 runs.
- `phylo_offline_take_check`: note-on frames, block-size independence.
- `phylo_audio_feed_check`: chunking, match to offline render, dropped blocks, stop, last write wins, control-thread churn.
- `phylo_playback_settings_check`: round trip, env fallback, `state=` key.
- `phylo_app_checks`: assistant settings and request timeouts.
- `phylo_providers`: provider adapters.

ThreadSanitizer: run directly with `g++ -fsanitize=thread` over the relevant sources. Feed and renderer checks are clean.

CI: `.github/workflows/ci.yml` on Ubuntu, macOS and Windows. Builds core and app, runs core tests. Green on every merged PR from #21 on.

**Commands used for GitHub work (REST, since GraphQL is blocked):**
- Create PR: `gh api repos/AdairBear/Phylotone/pulls -X POST -f title=... -f head=... -f base=... -F body=@file`
- Merge: `gh api repos/AdairBear/Phylotone/pulls/N/merge -X PUT -f merge_method=merge`
- Retarget base: `gh api repos/AdairBear/Phylotone/pulls/N -X PATCH -f base=...`
- CI status: `gh api repos/AdairBear/Phylotone/commits/<sha>/check-runs`. `mergeable_state: clean` means ready.

## 6. Milestone status

| Milestone | Status | Still open |
|---|---|---|
| M0 Skeleton | Built. Core sequencer, app shell, transport, MIDI out, CI. | Audible MIDI check. Timing uses a 1 ms sleep loop, so jitter is several ms (about 15 ms on Windows). The audio-driven clock is M1 work. |
| M1 Code layer | Scene parser first cut, next-bar changes, file watch. | Audible check (edit a pattern while it plays). Sections not played. Key not yet used. Immediate changes while stopped start at the playhead, not a bar. |
| M2 Assistant | Closed in code (PR #4, hardening #12, #13, per-change writes). | Accept check needs a person (change bass rhythm, hear it land on the bar, undo it). Live chat untested with any provider. Slow local models: 5-minute timeout not yet proven. |
| M3 Generator | Pads and bass, seeded, macros. Accept (same seed gives identical MIDI) is a core test. | Musical review. Voicings are simple. Audible check. |
| M4 Plugin host | Wire protocol, supervisor, renderer, offline render, feed, device output, Play path, plugin state. Akazi XL VST3 loads and renders in the host (verified with a scratch driver). | Sound through a device and through the app. Release host. The M4 sound check (see 8.3). Audio-thread leftovers (see 8.4). |
| M5 Mixer and recording | Core mixer and float WAV recorder. Bit-exact re-render at core level. | App wiring: levels, pan, master bus, record in UI. Waits for the audible M4 path. |
| M6 Drums and structure | Not started. | All of it. Sections are parsed but not played. |
| M7 Streaming | Not started. | All of it. |
| M8 Completion | Not started. | Contract quality bars, 4-hour soak, demo track built from docs by someone else. |

## 7. Akazi XL (separate repo)

- The plugin is the first instrument target. Its sampler outputs silence until a sample is in its state. Loading a sample is done through state, with `sourcePath` pointing at a WAV in the scratch test. The protocol has no sample message yet.
- Its `CLAUDE.md` sets the rules, which apply to any work there:
  - **Clean-room (hard):** never read, fetch or paste code from Akaizer (Ben Burchett), OpenKaizer, Akai firmware, or any other implementation of the Akai timestretch. Work from `docs/ALGORITHM.md` and the maths.
  - **Trademarks:** "Akai" and the S-series model names may appear only in the README heritage and credits section. Never in product names, plugin IDs, UI text or marks.
  - Colours come from the skin (`design/skin.json` to `tools/gen_skin.py` to `plugin/src/skin/AkaziXLSkin.h`). Never hard-code colours. Akazi Connect is GPL-3.0: reuse its token values, never its source.
  - Read `tasks/lessons.md` in that repo.
- Its build: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DFETCHCONTENT_SOURCE_DIR_JUCE=$HOME/SDKs/JUCE`. Goldens are regenerated only when an engine change is meant to change the sound, and the commit says so.
- The Akazi XL VST3 used in tests was built from its repo with JUCE 8.0.15 into a scratch directory. Nothing was committed to its repo.

## 8. Next work, in order

### 8.1 Phylotone sample library (the agreed next build)

Spec: `docs/SAMPLE_INTERFACE.md`, merged in #25. Branch from `kickoff/contract`.

1. **Library folder and index.** `Documents/Phylotone/samples/`, one file per sample, plus an index so a sample keeps its name and slot across restarts. Slots 0 to 127.
2. **Import.** Accept WAV at 16, 24 or 32-bit float, mono or stereo. Convert to 48 kHz, 32-bit float. Refuse anything longer than 60 s in the app, before it is sent.
3. **Slot assignment.** Phylotone decides the slot number. Expose a way to assign or clear a slot in the UI or scene.
4. **Slot blob.** Encode the full slot set as one `akazi.samples.v1` state blob (format in the doc). Core encoder and decoder, with tests for every field and for each refusal case in the doc.
5. **Send.** Send the blob through the existing SetState path on host start and on every slot change. Check the StateResult text: empty means accepted, otherwise show the reason on the status line.
6. **Stopgap.** The current `state=` file in `playback.txt` stays, labelled as a stopgap.

**Accept:** drop a WAV in; it appears in the library; assign it to slot 0; Plugin audio on; the blob reaches the host and the StateResult is empty. Then a deliberately bad blob (wrong header, 61 s file, slot 128) is refused with the reason shown, and the previous slots keep playing. Headless check for the blob codec and the refusal cases. The audible part needs a person.

**Open decisions (need Thomas):**
- Root note per slot. Proposed: optional, default middle C.
- Memory budget for 60 s samples. Needs a check in Akazi XL.
- Whether StateResult reports the loaded slots. Proposed: v2.

### 8.2 Recording and drum programming (after 8.1)

- Recording writes a WAV into the same library, then it is a slot like any other.
- Drum programming is in Akazi XL's sampler. Phylotone sends notes and slots only. Sound design stays in Akazi XL.

### 8.3 M4 sound check (needs a person with audio out)

- Plugin audio on, Play. Expect sound from the Akazi XL state file.
- Kill the host during playback. The app must keep running and restart the host.
- Check the Release build of the host.

### 8.4 M4 audio-thread leftovers

- `buildBlock` allocates a request each block.
- `AudioFeed::pull` grows its `ready_` deque.
- A replaced pattern's note storage is freed on the audio thread. Move it to the deferred free.

### 8.5 M5 app side

Levels, pan, master bus, and record in the UI, on the same output path as M4.

### 8.6 M6 to M8

Per the milestones. Sections played (M6), streaming (M7), and the completion bars (M8, including the 4-hour soak).

### 8.7 Akazi XL change list (needs approval; separate repo)

Read the `akazi.samples.v1` blob in SetState, map slots to the sampler, answer StateResult (empty on success, reason on refusal), and document the slot-to-sampler mapping. Do not start this without Thomas's go-ahead.

## 9. Open decisions and known gaps

- **Scene syntax** is Phylotone's own, not Strata-like. The contract lists it as open. Change it if Thomas wants Strata.
- **Key** is stored but not yet used. Planned for M3 and still open.
- **Sections** are parsed but not played (M6).
- **Assistant:** the scene file is written per change. Approve and reject are sent to the model as notes. Not yet confirmed with a live model.
- **Naming:** "Phylotone" is a placeholder. A naming session is planned.
- **Audio thread:** the device callback is lock-free now, but the leftovers in 8.4 remain.
- **Plugin state:** a state file must contain a sample for Akazi XL to sound. There is no in-app way to make one yet, which is the reason for 8.1.

## 10. Environment limits (for whoever picks this up)

- No audio device, no ALSA sequencer, no MIDI device on the build machine.
- No Akazi XL plugin on the build machine. The VST3 was built in a scratch directory outside both repos.
- No API keys or network for live assistant calls.
- Scratch driver scripts (for the state and render tests) live outside the repo under `/root/scratch-m4`. They are not part of the project. Do not rely on them in CI.
- Ideally, the person picking this up runs the audible checks on a machine with audio and MIDI, and reports back.

## 11. Pointers

- `docs/CONTRACT.md`: the completion contract (reference).
- `docs/MILESTONES.md`: the milestone path and accept checks.
- `docs/ASSISTANT.md`: the assistant spec. Part of the product, not an add-on.
- `docs/SAMPLE_INTERFACE.md`: the sample interface proposal (v0.1, merged in #25; Akazi XL side not yet agreed).
- `CONTINUATION.md`: session log and PR history. Its "Next" list is superseded by section 8 here.
