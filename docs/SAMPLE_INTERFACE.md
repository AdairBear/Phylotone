# Phylotone to Akazi XL: sample interface

*Draft v0.2, proposal. Akazi XL's side is not agreed yet. v0.2 adds the byte layout that
Phylotone's side now implements (see "Byte layout"), and the points found while building
it (see "Found while building"). v0.1 was the first proposal, merged in #25.*

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

### Byte layout

All integers are little-endian. Floats are IEEE 754 bit patterns. The reference is
`core/include/phylo/samples/SampleBlob.h`; the encoder, the decoder and their tests are in
`phylo_core`.

| Offset | Size | Field |
|---|---|---|
| 0 | 16 | Header: the ASCII bytes `akazi.samples.v1`, no terminator |
| 16 | 4 | u32 sample rate in Hz. Always 48000 |
| 20 | 4 | u32 slot count N, 0 to 128 |
| 24 | | N slot records, in strictly ascending slot order |

One slot record:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | u8 slot number, 0 to 127 |
| 1 | 1 | u8 root note, 0 to 127. 60 (middle C) when not set. See open question 1 |
| 2 | 1 | u8 channel count C, 1 or 2 |
| 3 | 1 | u8 name length L in bytes, 0 to 64 |
| 4 | 4 | u32 frame count F, 1 to 2 880 000 (60 s) |
| 8 | L | Name, UTF-8, no terminator |
| 8 + L | 4·F·C | float32 samples, interleaved |

The blob ends with the last record. A blob with N = 0 is valid and clears every slot.

Samples are any finite float. Values above 1.0 are allowed: a float source can hold them,
and conversion to 48 kHz can overshoot a full-scale source by a few tenths. Phylotone does
not clip. NaN and infinity are refused.

A receiver checks in this order and refuses with the first problem: header, total size
(see "Size"), sample rate, slot count, then for each slot: slot range, slot order, root note, channel count, name
length, frame count (zero, then over the limit), truncation, name is UTF-8, samples are
finite. Last: bytes after the final record.

### Size

Version 1 sends the set as one SetState frame, so the whole blob must fit in one wire
payload. That limit was 16 MiB, which is less than one 60 s stereo sample (about 23 MB).
It is now 64 MiB (`kMaxPayloadBytes`). Phylotone refuses to build a larger set and says so
on the status line. 64 MiB is about 5 min 49 s of mono, or 2 min 54 s of stereo, across
all slots. See open question 2.

Version 2, if needed, would add a per-slot message so a single change does not resend
the whole set. Not part of v1.

## Operations (what the plugin does)

- **Accept a slot set.** Replace all slots with the ones in the blob.
- **Refuse a slot set.** Return a reason and keep the current slots. Refusals: unknown
  format header, a sample over the length limit, a slot number out of range, a sample
  rate other than 48 kHz, a truncated blob. The byte layout adds the rest of the checks
  (see "Byte layout").
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

## Found while building (Phylotone side, v0.2)

These came up in the build. None is settled.

1. **A raw blob does not reach a VST3 plugin through JUCE's host.** `phylo-plughost` calls
   the hosted plugin's `setStateInformation`. For VST3, JUCE's host side only accepts its
   own XML wrapper there and drops any other bytes without an error. So the raw blob never
   arrives at Akazi XL today, and the plugin cannot answer through that call either: it
   returns nothing. The stopgap `state=` file works because it holds bytes the plugin made
   through the same wrapper.
2. **So the host answers StateResult for a sample blob, not the plugin.** `phylo-plughost`
   recognises bytes that start with `akazi.samples.`, checks them with the same decoder,
   and answers the refusal reason itself. A refused blob is not passed to the plugin, so
   the plugin keeps what it had. A valid blob is passed on as before.
3. **Still to agree with Akazi XL:** how a valid blob is delivered to the plugin (for
   example, the host wraps it as the VST3 component state so it arrives in the plugin's
   `setStateInformation`), and how the plugin reports a refusal of its own (for example
   out of memory). Until then an empty StateResult means "the host accepted the blob",
   not "Akazi XL loaded it".
4. **The two state paths are kept apart.** The stopgap `state=` bytes and the sample blob
   are stored and re-sent separately after a host restart, the plugin state first.

## Open questions

1. Should a slot carry a root note, so the sample plays at the right pitch without the
   user setting it? Proposed: yes, optional, default middle C.
2. Is 60 seconds enough for drum and loop sampling? Needs a check on Akazi XL's memory
   budget. The same check should set the budget for the whole set (64 MiB in v0.2, see
   "Size").
3. Should Akazi XL report the slots it has loaded in StateResult, so Phylotone can show
   them? Proposed: not in v1; v2 with the per-slot message.
