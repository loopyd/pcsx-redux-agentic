# Mixed PCM and SPU access capture

Run `--mission spu --argument action=capture` ([source](../../../../src/lua/missions/spu.lua))
for bounded mixed PCM/SPU accesses. Missing `PCSX.Audio` fails; no hardware-fidelity claim.

## Procedure

Supply state and matching origin receipt, or executable/target. Required
`identity=LABEL`: 1–160 bytes; receipt identifies input/tool hashes.

| Argument | Meaning / bounds / default |
| --- | --- |
| `frames` | GPU Vsync bound before the primary stop; 1–36000 / 60 |
| `stop` | Optional aligned PC; frame exhaustion before arrival fails |
| `tail` | Additional guest Vsyncs after primary stop; 0–3600 / 0 |
| `samples` | Storage capacity in stereo PCM frames; 1–2646000 / 441000 |
| `blocks` | Callback record capacity; 1–65536 / 16384 |
| `events` | Separate SPU event and worker-batch capacities; each 1–65536 / 16384 |

Other SPU action arguments reject. Capacity overflow fails; it is not a duration
stop. Set callback capacities and GPU Vsync limits according to their different
timing.

Save initial state before begin, final state after freeze. Never save, restore,
reset or edit during capture. Maintained helpers reject active history/audio;
arbitrary Lua writes are not intercepted. Audit scripts and avoid GUI/settings
changes. Existing queues and partial mixer work are retained, not flushed.

CPU pause precedes deferred freeze; audio may append between them. `tail`
continues guest execution without key-off, DSP synchronization or guaranteed
release completion. Unexpected pause, loss, overflow or export errors fail
without a success marker. Inspect retained files and logs.

| Capture | Content |
| --- | --- |
| `audio.f32`, `audio.wav` | Identical interleaved stereo float32 little-endian payload; WAV uses IEEE-float format and a sample-count `fact` chunk |
| `blocks.ndjson` | Callback index, output offset/count, absolute voice source offset, dequeued voice/disc counts, flags and observed CPU publication |
| `workers.ndjson` | Completed worker-batch source ranges, started-voice masks and acknowledged enqueue ranges |
| `spu.ndjson` | Native named access/feed records, exact cycles, observed frame cursor and parent identity |
| `audio.json` | Native counts/limits/settings/queues, failure/drop data, export checks and retained PCM duration |
| `sound.json` | Mission bounds, primary/tail observations, stop reason and outcome |
| `initial.pbuf`, `final.pbuf` | Machine contexts surrounding capture; neither contains the complete host audio queues |

For custom missions, begin on a paused owner-thread interpreter. Paused-owner
stop/export/clear can recover failed evidence after backend changes; `status`
requires owner thread, not pause.

| Operation | Contract |
| --- | --- |
| `begin(samples, blocks, events)` | Paused interpreter, empty recorder, initialized stream, positive capacities, no MSAN |
| `stop()` | Freeze active recorder, retaining failures; defer outside device/Pause callbacks |
| `status()` | Owner-thread snapshot; only frozen state has a terminal end cycle |
| `blocks(first, count)`, `workers(first, count)`, `events(first, count)` | Frozen capture; zero-based range, at most 1024 rows; owning one-based named tables |
| `samples(offset, frames)` | Frozen capture, at most 65536 stereo frames per call, copied binary string |
| `exportPCM(raw, wav)` | Stream the frozen complete range to distinct simple capture filenames; return frame/raw/WAV byte counts |
| `clear()` | Release inactive capture explicitly before another begin |

Record ranges validate before copying; zero rows at the retained end are valid.
PCM export accepts ASCII filenames of 1–255 bytes, starting with a letter or digit,
then letters, digits, `_`, `.` or `-`. It writes 2048-frame chunks without a full
Lua PCM string, preserving float bits and the 58-byte IEEE-float WAV header. Files
close before success or error; failed output prefixes remain on disk. Successful
`pcm.export` registers both captures. `pcm.export(prefix)` selects a simple
filename prefix for repeated captures. It allows at most 200 ASCII letters,
digits, `_`, `.` or `-`, starting with a letter or digit. Its returned `files`
names each raw, WAV,
block, worker, event and report capture. [Replay](replay.md#native-sound-comparison) uses
this form for bounded segments and freezes before checked state operations. Failed frozen recorder prefixes remain readable.

Native Audio ABI and `psx.native-audio` report version: 3. The 14 signed 64-bit
CD environment slots remain at the same offsets: `volume`, `interpolation`,
`key_on_delay`, `mute`, `mono`, `streaming`, `null_sync`, `schema_version=2`,
`reserved=0`, `speed`, `forced_irq`, `voice_mute`, `voice_solo`, `xa`.
`speed` is the configured signed SPU sink multiplier; nonpositive values or values
above 1024 use the native unbounded cap of 1024. It is not a CPU cycle scaler.
`key_on_delay` is the configured signed startup sample count; the native voice
producer treats negative values as zero. Retain configured values without
normalizing them. `reverb`, `irq_wait`, `decode_irq` and `scaler` have no equivalent
in this fork. Revision markers reject incompatible Audio/CD environment receipts.
CPU clock is exposed separately as
`PCSX.CONSTS.CPU.CLOCKSPEED` (33868800); all 11 scheduler scale bits are captured
by the CD recorder. These settings do not prove DSP or hardware fidelity.

States: empty=0, active=1, frozen=2. Keep uint64 fields as canonical decimal strings,
never doubles. Failure bits: frames=1, blocks=2, events=4, thread=8, bounds=16,
clock=32, discontinuity=64, streaming-loss=128, output=256, workers=512. Failures are sticky,
drop counters saturate, buffers never overwrite.

## Application

Capture stage: **44100 Hz stereo float32 after mixer/mute/mono, before SDL
conversion**. Voice/reverb includes host Volume scaling and signed16 clipping;
XA/CDDA uses native resampling/scaling before callback summing/mute/mono. Preserve
values beyond ±1 without clipping/normalizing. SDL/device resampling, gain and
physical playback are excluded. Retain settings and active/configured driver/device.

Block offsets count stereo frames. Flags: mono=1, mute=2, successful SDL
submission=4. Short dequeues zero-fill; empty disc streams do not imply loss.
`zero_fill` totals cover validated block prefixes: check completeness. Enqueue/
submission losses set failure bits; failed prefixes may retain useful PCM.

`voice_offset` counts frames dequeued from stream 0 since the audio device owner
was created. It continues across capture epochs and device reinitialization.
For each callback, only its first `voice_frames` output frames consume this source
range. Underflow zeros consume no source frames. Fast-forward discards advance
the source coordinate without adding PCM. Source coordinates are observations of
the host queue, not restored PSX state or CPU sample deadlines.

Worker rows contain `index`, `voice_offset`, `frames`, `key_on_mask`,
`enqueue_offset` and `enqueue_frames`. Each bit in the 24-bit mask marks a voice
that actually entered `StartSound` in that batch. Enqueue fields identify the
successful stream-0 submission containing the complete batch. Zero
`enqueue_frames` means unconfirmed; exclude that row from signal comparisons.
Worker storage has the supplied event capacity, independently of event storage.
`workers`, `worker_limit` and `dropped_workers` report its retained count, capacity
and loss. Frozen exports copy at most 1024 rows under the worker mutex before
allocating Lua tables.

Capture does not stop the worker. A batch already in progress at begin, or still
in progress at freeze, has no retained row. Freeze can also precede the worker's
acknowledgement of an already consumed enqueue. Such prefixes and unconfirmed
tails remain unknown. Separate producer, consumer and queue snapshots have no
atomic admission boundary. For a bounded key-on comparison, establish an initial
clear pending-start flag and exactly one accepted KON for the selected voice,
with no competing host writes. The observed start mask then identifies its batch.
Retriggers and overlapping CPU writes require additional evidence.

| Event kind | Native meaning |
| --- | --- |
| 1 | Bus write attempt |
| 2 | Effective write-handler observation |
| 3 | XA/CDDA feed: source rate/count/layout, output count and enqueue acceptance |
| 4 | Bus read attempt |
| 5 | Effective read-handler result |
| 6 | Bus returned value |

Non-feed kinds include address/value/width. Parent IDs connect dispatcher/handler;
zero means no recorded parent, not necessarily guest root. Words may contain
halfword children. Preserve aliases, widths and returns; labels imply no extra accesses.

Block cycles use potentially stale CPU publications; event frame cursors mark
output observed by CPU. Neither locates audible effects or per-sample CPU cycles.
Confirmed worker source ranges can join callback source ranges. Missing worker
rows and queue snapshots leave prehistory and remaining work unresolved; partial
batches and host scheduling remain uncontrolled. Freeze proves neither queue drain,
sound end, complete CD lifecycle nor determinism. Independently validate signals,
register sequences and failures before semantic acceptance.
