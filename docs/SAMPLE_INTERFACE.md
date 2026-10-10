# Phylotone to Akazi XL: sample interface

*Draft v0.1, proposal. Akazi XL's side is not agreed yet. Nothing here changes the
host protocol until both sides sign it off.*

## Purpose

Phylotone gives Akazi XL samples from several sources: a file dropped in, a recording
made in the app, or a rendered take. Akazi XL owns the sampler: how a sample is stored,
mapped and played. Phylotone owns the library of files and decides which sample goes to
which slot. Neither side needs to know the other's internals.

## Roles

- **Phylotone** keeps a sample library, converts each sample to the format below, and
  sends a set of slots to the plugin when it starts and when a slot changes.
- **Akazi XL** accepts the slots, maps them to its sampler, and reports what it loaded or
  refused. It never reads the library folder.

## Sample format

- WAV, 16-bit, 24-bit or 32-bit float, mono or stereo on import.
- Phylotone converts to **48 kHz, 32-bit float, mono or stereo** before sending. Akazi XL
  receives only this format and does not resample.
- Length limit: 60 seconds per sample. Longer files are refused in the app, not sent.
- Phylotone sends the sample bytes, not a path, so the state is self-contained. A path
  would break when the project moves between machines.

## Slots

- A slot is identified by an integer from 0 to 127.
- A slot holds one sample: a name, the channel count, the frame count and the float data.
- A slot with no sample is empty. Akazi XL plays silence for an empty slot.
- Phylotone decides the slot number. Akazi XL does not reorder or rename slots.

## Transport (version 1)

The existing host protocol carries plugin state (SetState, StateResult). Version 1 uses
it. Phylotone sends the full slot set as one state blob:

- A header that names the format: `akazi.samples.v1`.
- For each non-empty slot: the slot number, the name (UTF-8, at most 64 bytes), the
  channel count, the frame count, and the float32 samples, little-endian, interleaved.
- The blob is a full replacement. Phylotone sends every non-empty slot each time, so a
  slot that is missing from the blob is cleared.

Phylotone may send the blob again at any time (tempo changes do not need it). Akazi XL
answers with a StateResult. An empty text answer means the blob was accepted. A text
answer is the refusal reason, for the status line.

Version 2, if needed, would add a per-slot message so a single change does not resend
the whole set. Not part of v1.

## Operations (what the plugin does)

- **Accept a slot set.** Replace all slots with the ones in the blob.
- **Refuse a slot set.** Return a reason and keep the current slots. Refusals: unknown
  format header, a sample over the length limit, a slot number out of range, a sample
  rate other than 48 kHz, a truncated blob.
- **Keep the sample when the host restarts.** Phylotone sends the blob again after a
  host restart, so Akazi XL does not need to store anything on disk.

## Not part of this interface

- Recording. Phylotone records and saves a WAV, then sends it as a slot.
- Editing or programming a sound (envelopes, filters, drum synthesis). That belongs to
  Akazi XL's own editor. Phylotone sends notes; it does not send sound-design parameters
  in v1.
- Choosing a sample for a note. The mapping from notes to slots stays in Akazi XL.

## Phylotone side (what the app adds)

- A sample library folder under Documents/Phylotone/samples, with one file per sample.
- A library index, so a sample keeps its name and slot across restarts.
- Import of a dropped WAV: convert, check the length, store in the library, offer a slot.
- A stopgap until v1 exists: the current `state=` file in playback.txt. It works only
  with the state the plugin makes, so it stays labelled as a stopgap.

## Akazi XL side (what the plugin must add)

- Read the `akazi.samples.v1` blob in SetState and map its slots to the sampler.
- Answer StateResult with an empty text on success, or the refusal reason.
- Document the slot-to-sampler mapping in Akazi XL's own docs.

## Open questions

1. Should a slot carry a root note, so the sample plays at the right pitch without the
   user setting it? Proposed: yes, optional, default middle C.
2. Is 60 seconds enough for drum and loop sampling? Needs a check on Akazi XL's memory
   budget.
3. Should Akazi XL report the slots it has loaded in StateResult, so Phylotone can show
   them? Proposed: not in v1; v2 with the per-slot message.
