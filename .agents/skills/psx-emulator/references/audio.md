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
| `events` | Independent SPU, worker, gain, envelope-input and scheduling capacities; each 1–65536 / 16384 |

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
| `gains.ndjson` | First consumed dry stereo gain pair per voice and later changed pairs, at absolute voice-source frames |
| `envelope_inputs.ndjson` | First consumed ADSR phase controls per voice and later changes, at absolute voice-source frames |
| `scheduling.ndjson` | Ordered CPU, producer, queue and master-clock observations |
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
| `blocks(first, count)`, `workers(first, count)`, `gains(first, count)`, `events(first, count)` | Frozen capture; zero-based range, at most 1024 rows; owning one-based named tables |
| `envelopeInputs(first, count)` | Same frozen range contract; phase-specific consumed controls |
| `scheduling(first, count)` | Same frozen range contract; shared scheduling observation order |
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
names each raw, WAV, block, worker, gain, envelope-input, scheduling, event and report capture.
[Replay](replay.md#native-sound-comparison) uses
this form for bounded segments and freezes before checked state operations. Failed frozen recorder prefixes remain readable.

Native Audio ABI and `psx.native-audio` report version: 10. The 14 signed 64-bit
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
clock=32, discontinuity=64, streaming-loss=128, output=256, workers=512,
gains=1024, envelope-inputs=2048, scheduling=4096. Failures are sticky, drop counters saturate,
and buffers never overwrite.

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
`enqueue_offset`, `enqueue_frames`, `pre_sample_steps`, `stop_samples` and
`first_reverb_sample`. Each bit in the 24-bit mask marks a voice
that actually entered `StartSound` in that batch. Enqueue fields identify the
successful stream-0 submission containing the complete batch. Zero
`enqueue_frames` means unconfirmed; exclude that row from signal comparisons.
Worker storage has the supplied event capacity, independently of event storage.
`workers`, `worker_limit` and `dropped_workers` report its retained count, capacity
and loss. Frozen exports copy at most 1024 rows under the worker mutex before
allocating Lua tables.

`pre_sample_steps` contains 24 signed 32-bit installed interpolator steps in
16.16 units. Active channels record after ordinary frequency installation, before
the sample loop; inactive channels record before their silent walk. Channels are
observed in sequence. These values are not a simultaneous snapshot or proof of
sample consumption. Startup silence can retain a step, never-started channels can
report zero, and modulation can replace a step inside the loop. The worker journal
has a 48 MiB export bound; block and event journals each retain 16 MiB bounds.
The harness capture and replay aggregate limits still apply.

`stop_samples` contains 24 signed batch-relative sample indices. Each channel
records the first sample whose existing ADSR call consumes a true stop input;
`-1` means no such call occurred. Startup delay, inactive walking and stopped
fetches do not call ADSR. A persistent stop can appear in successive batches,
so this is neither a new key-off event nor proof of release completion. It adds
no device read and does not locate the CPU write in sample time.

`first_reverb_sample` is the first batch-relative sample whose existing reverb
base check admits work; `-1` means none. It observes that branch without another
base read or a filter/cursor snapshot. A valid base advances filters and reads
RAM even with master write enable clear. This index is neither a new activation
edge nor evidence that later samples remain active. Join it to an acknowledged
worker range. A first-activation comparison also needs independently established
fresh filters, an initially disabled base and exactly one guest activation.

`gains` records `index`, `voice_offset`, `voice`, `left` and `right`. Voice indices
are zero-based. Signed 32-bit gains are the effective levels consumed by the dry
stereo multiplications, rather than raw register values. The first consumed pair
per voice in an epoch is retained, followed by changed pairs. Each side is copied
at its existing read, in the original left-multiply/right-multiply order. The pair
is not an atomic snapshot. No extra gain read or DSP state import occurs.

Inactive voices, startup delay, modulation sources and debug-muted voices do not
read dry gains. They produce no gain observation. Source offsets increase within
each voice; records follow channel processing order and need not increase across
voices. Reverb gain reads remain separate. Join each gain's source frame to a
completed, acknowledged worker range before using it in a signal comparison.
Crossing prefixes, incomplete batches and unconfirmed enqueue tails remain unknown.
These observations locate consumed dry inputs, not CPU writes or hardware latency.

Gain storage has a separate capacity equal to the supplied event limit.
`gains`, `gain_limit` and `dropped_gains` report its count, bound and loss. Exports
copy at most 1024 records under the worker mutex before allocating Lua tables.
The gain journal has a 16 MiB byte bound. Overflow sets the sticky `gains` failure,
and a failed frozen prefix remains readable.

`envelopeInputs` records `index`, `voice_offset`, `voice`, `phase`, `rate`,
`exponential`, `increasing` and `sustain_level`. It copies controls from the
envelope's existing reads without additional device reads. Modes are integers
zero or one. The rate is the raw phase-specific value before exponential-knee
adjustment. Only decay consumes `sustain_level`; other phases report `-1`.

| Phase | Value | Rate range | Fixed controls |
| --- | --- | --- | --- |
| Attack | 0 | 0–127 | Increasing |
| Decay | 1 | 0–15 | Exponential, decreasing; sustain level 0–15 |
| Sustain | 2 | 0–127 | None |
| Release | 3 | 0–31 | Decreasing |

The journal retains each voice's first consumed controls in an epoch and later
changes, including phase changes. It contains no envelope level or counter.
Separate field reads are not an atomic register snapshot. Startup delay,
inactive walking and terminated fetches do not step ADSR and emit no row.
Modulation-source and debug-muted voices still step ADSR and can emit rows.
Per-voice order, worker-range joins and unknown prefixes/tails follow the gain
contract above. These inputs locate consumption, not CPU writes or hardware latency.

Envelope-input storage independently uses the supplied event capacity.
`envelope_inputs`, `envelope_input_limit` and `dropped_envelope_inputs` report
its count, bound and loss. Frozen range reads copy at most 1024 rows under the
worker mutex before Lua allocation. The journal has a 16 MiB export bound.
Overflow sets sticky `envelope-inputs` failure; failed prefixes remain readable.

`scheduling` records zero-based `index` and `kind` in one append order across CPU,
producer, callback and null-clock threads. The recorder copies existing operands
without extra device reads. Its mutex protects append, freeze and frozen copying,
including the null-clock writer. No recorder lock spans a wait or DSP operation.

| Kind | Retained operands |
| --- | --- |
| 1, producer boundary | `voice_offset`, `enqueue_offset`, `staged_frames`, derived `batch_phase`, `frames` |
| 2, enqueue entry | `voice_offset`, `frames`, `queued_before`, `blocked` |
| 3, enqueue result | `voice_offset`, `frames`, `queued_after`, `wait_nanoseconds`, `accepted` |
| 4, dequeue | `voice_offset`, `requested_frames`, `frames`, `queued_before`, `queued_after`, `discard` |
| 5, master advancement | `frames`, effective `speed`, `old_frames`, `new_frames`, `goal`, `action`, `source` |
| 6, CPU decision | `cycle`, `previous_cycle`, `previous_audio_frames`, `target`, sampled `master_frames`, `action` |
| 7, wait entry | `cycle`, published `goal`, `initial_trigger` |
| 8, wait return | The same retained entry operands, without another trigger or master read |

Producer boundaries identify the next 45-frame batch. `staged_frames` counts
completed frames still awaiting submission, and `batch_phase` is that count divided
by 45. The first boundary is observed on the producer thread after begin. It is
not an atomic begin snapshot, and an already-running batch remains an unknown prefix.
Join source ranges to completed, acknowledged worker rows before comparing signals.

Enqueue and dequeue observations hold the voice queue's existing mutex. The strict
admission predicate requires queued plus incoming frames below 2048. Entries and
results can surround dequeues while the existing 200 ms wait releases that mutex.
Wait duration has the same host-time meaning as feed waits. `discard=1` marks
fast-forward consumption that produces no retained PCM.

Master counters wrap as uint32. `source=0` is the callback; `source=1` is the null
clock. Actions are unchanged goal=0, pending goal=1 and trigger increment before
notification=2. The wake predicate compares `goal` with the old `fetch_add` total,
not `new_frames`. CPU actions are no wait=0, wait=1 and debt adjustment=2.
Wait entry follows goal publication; return records completion of the existing wait.

The append order orders observations outside the queue mutex. It does not make
master atomics, CPU loads or goal publication atomic with recording, and it does
not locate CPU writes in sample time. Unfinished enqueue and wait pairs at freeze
remain visible in `pending_scheduling`. Scheduling storage independently uses the
event capacity. `scheduling`, `scheduling_limit` and `dropped_scheduling` report its
count, capacity and loss. Frozen ranges copy at most 1024 rows before Lua allocation.
Export has a 32 MiB bound; overflow sets sticky `scheduling` failure. Failed prefixes
remain readable, and the harness capture and replay aggregate limits still apply.

Capture does not stop the worker. A batch already in progress at begin, or still
in progress at freeze, has no retained row. Freeze can also precede the worker's
acknowledgement of an already consumed enqueue. Such prefixes and unconfirmed
tails remain unknown. Begin/end producer, consumer and queue snapshots have no
common atomic boundary. For a bounded key-on comparison, establish an initial
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
| 7 | Feed wait: queue occupancy, initial capacity predicate and host duration |

Each feed has an adjacent kind-7 record with `parent` set to its feed ID and the
same CPU cycle. `queued_before` and `queued_after` are sampled under the queue
mutex, before waiting and after admission or rejection. The latter includes an
accepted batch. `blocked=1` means the initial queued plus incoming count was at
least 16,384; it does not prove an OS sleep. `accepted` matches the parent feed.
`wait_nanoseconds` is a decimal uint64 duration around `wait_for`, including
mutex reacquisition and excluding initial lock acquisition, copying and export.
It is zero for immediate admission. The existing strict capacity predicate and
200 ms deadline are unchanged. Host duration is not emulated time, and these
observations do not establish a CPU-to-output sample mapping. A rejected feed
and its wait record precede the sticky streaming-loss failure; later rows can be
missing. Preserve failed captures and their loss counters.

Bus kinds include address/value/width. Parent IDs connect dispatcher/handler;
zero means no recorded parent, not necessarily guest root. Words may contain
halfword children. Preserve aliases, widths and returns; labels imply no extra accesses.

Block cycles use potentially stale CPU publications; event frame cursors mark
output observed by CPU. Neither locates audible effects or per-sample CPU cycles.
Confirmed worker source ranges can join callback source ranges. Missing worker
rows and queue snapshots leave prehistory and remaining work unresolved; partial
batches and host scheduling remain uncontrolled. Freeze proves neither queue drain,
sound end, complete CD lifecycle nor determinism. Independently validate signals,
register sequences and failures before semantic acceptance.
