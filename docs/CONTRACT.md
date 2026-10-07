# Phylotone: completion contract

*Status: draft v0.1, 2026-10-07. Edit freely. This is the fixed reference the milestones are measured against.*

## 1. End state

A standalone music-making app for coding music, with the sound design and
structure of a full song, that does not need a full DAW. You write or edit short
scenes in code, a built-in generator performs them, and you can host third-party
instruments and effects (Akazi XL is the first target). The app records what you
play and can stream it.

It is a host, not a plugin. It loads VST3, AU and CLAP plugins.

## 2. Capabilities at completion

1. **Code layer.** A small scene language: harmony, scales, tempo, sections,
   patterns, and bindings from macros to parameters. Edits take effect on the next
   bar, with no audio dropouts.
2. **Generator.** Voice rules for pads, strings, bass, piano, drones and synth
   textures. Drum voices in two backends: synthesized, and sliced samples.
3. **Macro controls.** A small set (density, tension, motion, register, space,
   drift, and per-section additions). Each can be driven by a knob, MIDI CC or
   the assistant.
4. **Song structure.** Sections (intro, verse, build, drop, breakdown, outro) with
   their own harmony and active voices. Arrangement is a timeline of sections.
5. **Plugin host.** Load instruments and effects per track. Crashes in a plugin
   must not take the app down.
6. **Mixer and transport.** Tracks, levels, pan, sends, a master bus, a shared
   clock that follows the host transport when one exists.
7. **Recording.** Record the master bus or any track to WAV, sample-accurate and
   matching the live output.
8. **Streaming.** Live output to an Icecast or RTMP destination, or capture by OBS
   as a fallback.
9. **Assistant agent.** A capable agent available inside every project, whether or
   not music is playing (see section 5).

## 3. Non-goals

- Full arrangement editing with a piano roll and automation lanes.
- Audio recording of live instruments (microphone input).
- Mixing-console depth beyond the capabilities in section 2.
- Mobile.
- Store distribution in the first release.

## 4. Quality bars

- **Audio thread.** No allocation, locking or blocking in the render path. Checked
  by a test that counts allocations, as Akazi XL does.
- **Latency.** Keyboard to sound within one audio buffer plus the plugin's own
  latency, which the host compensates for.
- **Timing.** Scene edits land on the bar boundary within one buffer.
- **Stability.** A 4-hour soak test with recording and streaming on, no dropouts
  and no leaks.
- **Plugin safety.** A misbehaving plugin is isolated and can be unloaded without
  restarting the app.
- **Reproducibility.** Seeded generation, so a take can be replayed exactly.
- **Parity.** Offline render matches the live output, checked by a parity test with
  negative controls, as in Akazi XL.

## 5. Assistant agent (required)

- **Availability.** Always reachable from the app, from the project folder, and
  from a chat. It does not depend on audio playing.
- **Knows the project.** It reads the scene files, the arrangement, the mix and the
  take history.
- **Can act.** Edit a scene, change a macro, generate a pattern, render a preview,
  start or stop recording, list instruments. Each action is a named tool with a
  clear scope.
- **Asks before**: deleting material, overwriting a take, changing the master
  bus, or loading a plugin that has not been used in the project before.
- **Leaves a trail.** Every change it makes is logged with the reason it gave.
  Any change can be undone.
- **Persistent notes.** It keeps a per-project notes file so it can pick up where
  it left off.
- **Honest about limits.** It says when it does not know, and when a result is a
  guess.

## 6. Done means

All capabilities in section 2 work end to end on macOS, Windows and Linux, the
quality bars in section 4 pass, and a demo track can be composed, recorded and
streamed by someone other than the author using only the app and its
documentation.

## 7. Open decisions

- Working name: Phylotone (placeholder, not yet cleared; a full naming session is planned later).
- Scene language syntax: close to the Strata screenshots, or our own.
- Plugin hosting: JUCE's built-in host, or a separate process for each plugin.
- Assistant model and where it runs: locally, through the API, or both.
- Streaming first target: Icecast or RTMP.
