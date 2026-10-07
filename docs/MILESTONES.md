# Phylotone: milestones

Each milestone produces something you can run. Each ends with a hardening pass:
tests for the new code, a stability run, and a short note of what is still open.
The contract (CONTRACT.md) is the reference; milestones only move toward it.

## M0. Skeleton
- Standalone app builds on macOS, Windows and Linux, from a clean checkout.
- Transport (play, stop, tempo), one track, MIDI out.
- CI runs build and tests on all three platforms.
- **Accept:** press play, a test note sounds through a MIDI output.

## M1. Code layer
- Scene language parses: harmony, tempo, patterns, sections (first cut).
- Edits to a scene take effect on the next bar.
- **Accept:** change a pattern while it plays; the new pattern starts on the bar,
  with no dropout.

## M2. Assistant, first cut
- Assistant reads the project and can edit a scene, change a macro, and explain
  what a scene does.
- Every action is logged; any action can be undone.
- Works without audio running.
- **Accept:** ask the assistant to change the bass rhythm; the change is logged,
  plays on the next bar, and can be undone.

## M3. Generator
- First voices: pads and bass, following scene harmony.
- Macro knobs (density, tension, space) drive them.
- Seeded, so a take can be replayed exactly.
- **Accept:** the same seed and settings produce identical MIDI twice.

## M4. Plugin host
- Load an instrument (Akazi XL first) and effect slots per track.
- Misbehaving plugin is isolated; the app keeps running.
- **Accept:** load Akazi XL, play it from the generator; kill a test plugin and the
  app survives.

## M5. Mixer and recording
- Levels, pan, master bus.
- WAV recording of master or any track.
- **Accept:** record a take; an offline render with the same seed matches it
  within the parity tolerance.

## M6. Drums and song structure
- Synthesized drums and sliced-sample drums on one interface.
- Sections and an arrangement timeline.
- **Accept:** a three-section track (intro, drop, outro) plays with drums entering
  and leaving on section boundaries.

## M7. Streaming
- Icecast or RTMP output, or documented OBS capture.
- **Accept:** stream for 30 minutes to a test destination with no dropouts.

## M8. Completion
- All contract quality bars pass, including the 4-hour soak test.
- A demo track can be built and streamed by someone else from the docs.
