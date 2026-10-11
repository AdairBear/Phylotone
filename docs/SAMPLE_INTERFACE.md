# Phylotone to Akazi XL: sample interface

*v1.0, agreed on both sides (2026-10-11). Phylotone implements the host and the sender; Akazi XL
implements the plugin side. v0.2 added the byte layout; v1.0 adds how a blob is delivered to the
plugin and how the plugin answers (see "Delivery" and "The answer"), and records the decisions
of 2026-10-11 (see "Decisions"). v0.1 was the first proposal, merged in #25.*

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

Phylotone may send the blob again at any time (tempo changes do not need it). The host
answers with a StateResult. **An empty text answer means the plugin loaded the set.** A text
answer is the refusal reason, for the status line. How the plugin's answer reaches the host
is in "The answer". (Before v1.0 an empty answer only meant that the host had accepted the
blob. It now means the plugin loaded it, because the host can ask the plugin.)

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
| 1 | 1 | u8 root note, 0 to 127. 60 (middle C) when not set. See decision 1 |
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
all slots. See decision 2.

Version 2, if needed, would add a per-slot message so a single change does not resend
the whole set. Not part of v1.

## Delivery

How the blob gets from the host process to the plugin's `setStateInformation`.

**VST3.** JUCE's VST3 host accepts only its own XML wrapper through `setStateInformation` and
drops other bytes, so the host does not use that call. It hands the blob over as the VST3
**component state**, through JUCE's public extension API:

1. `AudioPluginInstance::getExtensions` gives an `ExtensionsVisitor::VST3Client`.
2. `getPreset()` returns the plugin's state as a VST3 preset container (the `.vstpreset`
   layout: `VST3`, version 1, a 32-digit class ID, the offset of a `List` of chunks such as
   `Comp` and `Cont`). The host reads the class ID from it.
3. The host builds a new container with that class ID and one `Comp` chunk holding the raw
   blob, and gives it to `setPreset()`.
4. JUCE's plugin wrapper passes component-state bytes unchanged to the processor's
   `setStateInformation`, so Akazi XL sees bytes that start with `akazi.samples.`. No base64
   and no XML around up to 64 MiB.

The container reader and builder are in `phylo_core` (`phylo/host/Vst3Preset.h`) and treat
their input as hostile: every offset and size is checked before use.

**Other formats.** The raw bytes go to `setStateInformation`, as for any plugin state.

**Order.** Phylotone sends the plugin's plain state first and the sample set after it, so the
slots win after a host start.

**Message loop and audio thread.** The plugin may apply a MIDI Program Change from a
message-thread timer, and that work can take a while (Akazi XL builds the new slot's sample
when it becomes the source: about half a second for a 60 s sample). `phylo-plughost` therefore
reads and splits its input on a second thread, runs the message loop on its main thread while
it waits, and renders Process blocks on a third (audio) thread, so a slow timer neither stalls
the timer from firing nor holds up the audio answer.

## The answer

`setStateInformation` returns nothing, so the plugin answers through its own state. The
plugin's state XML (what `getStateInformation` writes) carries three attributes on its root
element once a sample set has been received. They describe the **last** sample set received:

| Attribute | Value |
|---|---|
| `sampleSetBytes` | size of the received blob in bytes, decimal |
| `sampleSetHash` | FNV-1a 64-bit of every byte of the received blob, 16 lowercase hex digits |
| `sampleSetResult` | empty when the set was loaded; otherwise the refusal reason, one line |

FNV-1a 64: start with 14695981039346656037 (0xcbf29ce484222325); for each byte, xor the byte,
then multiply by 1099511628211 (0x100000001b3), modulo 2^64. (Check values: the empty input
hashes to `cbf29ce484222325`, the single byte `a` to `af63dc4c8601ec8c`.)

The attributes are absent until a sample set has arrived. They are written at save time only
and are ignored when an XML state is restored. Restoring an XML state does not touch the slots.

On SetState with bytes that start with `akazi.samples.` the host:

1. Checks the blob with the core decoder. A bad blob is refused by the host with the decoder's
   reason and never reaches the plugin, even when no plugin is loaded.
2. Delivers it (see "Delivery").
3. Reads the plugin's state back (VST3: `getPreset()` and the `Comp` chunk; other formats:
   `getStateInformation`), parses it with `AudioProcessor::getXmlFromBinary`, and reads the
   three attributes.
4. If `sampleSetBytes` and `sampleSetHash` match the blob just sent, StateResult is
   `sampleSetResult`: empty means loaded, otherwise the plugin's own reason. In every other
   case (attributes absent, no match, state unreadable) StateResult is
   `the plugin did not take the sample set`. That is what an old Akazi XL, or any other
   plugin, gets.

So **an empty StateResult for a sample set means the plugin loaded it.** Everything else about
SetState (plain plugin state, the `state=` stopgap) is unchanged.

For tests, the environment variable `PHYLO_HOST_SKIP_BLOB_CHECK=1` makes the host skip its own
blob check so that a bad blob reaches the plugin and the plugin's own refusal can be seen.

## Slot-to-sampler mapping (Akazi XL)

Defined in Akazi XL's `docs/SAMPLE_SLOTS.md`, which is the reference. In short, for v1:

- The plugin holds up to 128 slots (0 to 127). It plays **one active slot** at a time, as it
  played one sample before.
- The active slot is the automatable parameter **Sample Slot** (0 to 127, default 0). A MIDI
  **Program Change** n on any channel sets it to n (applied from a timer within about 50 ms,
  not sample-accurate).
- The slot's **root note** is the key that plays it at its own pitch. A slot that is empty
  plays silence. The last source wins: loading a file makes the file the source; an accepted
  set or a Sample Slot change makes a slot the source.
- Not in v1: several slots at once (a drum kit), per-note slot mapping, per-slot parameters.
  That is the next step.

## Operations (what the plugin does)

- **Accept a slot set.** Replace all slots with the ones in the blob.
- **Refuse a slot set.** Return a reason and keep the current slots. Refusals: unknown
  format header (any header that starts `akazi.samples.` but is not `akazi.samples.v1`), a
  sample over the length limit, a slot number out of range, a sample rate other than 48 kHz,
  a truncated blob. The byte layout adds the rest of the checks (see "Byte layout"). Running
  out of memory is a refusal too ("not enough memory for the sample set"), not a crash.
- **Keep the sample when the host restarts.** Phylotone sends the blob again after a
  host restart, so Akazi XL does not need to store anything on disk. The plugin never
  stores slot audio in its own saved state.
- **Always record the answer** for the last blob it received, accepted or refused (see
  "The answer").

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
- Record the answer (the three state attributes) for every sample blob it receives.
- Document the slot-to-sampler mapping in Akazi XL's own docs.

## Found while building (Phylotone side) and what was decided

These came up while building v0.2. All are settled in v1.0.

1. **A raw blob did not reach a VST3 plugin through JUCE's host.** `phylo-plughost` called the
   hosted plugin's `setStateInformation`. For VST3, JUCE's host side only accepts its own XML
   wrapper there and drops any other bytes without an error. **Decided:** the host delivers the
   blob as the VST3 component state through a preset container (see "Delivery"). Other
   formats get the raw bytes.
2. **The plugin could not answer through that call: it returns nothing.** **Decided:** the
   plugin records the answer in three attributes of its state XML and the host reads them back
   (see "The answer"). The host still checks the blob first; a refused blob does not reach the
   plugin, so the plugin keeps what it had.
3. **What an empty StateResult means.** It used to mean "the host accepted the blob". **Decided:**
   from v1.0 it means the plugin loaded the set. A plugin that did not record this exact blob
   (an older Akazi XL, or any other plugin) is reported as `the plugin did not take the sample set`.
   A refusal of the plugin's own, such as running out of memory, comes back as its reason.
4. **The two state paths are kept apart.** The stopgap `state=` bytes and the sample blob
   are stored and re-sent separately after a host restart, the plugin state first.

## Decisions (Thomas, 2026-10-11)

These were the open questions of v0.2. Thomas answered "yes to all three".

1. **Root note per slot.** A slot carries a root note: one byte, default 60 (middle C).
2. **Budget.** 60 seconds per sample, and 64 MiB for the whole set (see "Size").
3. **Loaded slots in StateResult.** Not in v1. Deferred to v2, with the per-slot message.

Delivery and the answer were agreed on 2026-10-11 and are described above. Thomas approved the
Akazi XL work for them on the same day (handoff 8.7).
