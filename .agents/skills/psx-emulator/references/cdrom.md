# CD-ROM observation

Run `--mission cdrom` ([source](../../../../src/lua/missions/cdrom.lua)) for
inspection, media reads, controller commands or native capture.

## Procedure

| `action` | Inputs / bounds |
| --- | --- |
| `inspect` (default) | State without target/EXE: offline; otherwise checked live controller |
| `directory` | Mounted `--disc CUE`; ISO `path` (root); at most 4096 entries |
| `file` | Mounted disc, required `path`; zero-based `offset` (0), `length` (remainder), at most 8 MiB and within file |
| `sectors` | Mounted disc; `lba=0..449849` (0), `sectors=1..1024` (1), `mode=RAW|M1|M2_RAW|M2_FORM1|M2_FORM2` (RAW) |
| `command` | `command=0..255`, `params` up to sixteen hex byte pairs, `frames=1..120` (60), bus `scratch`; optional `savestate=1` |
| `open-lid`, `close-lid`, `schedule-close-lid`, `mount`, `eject` | Checked live boundary; mount needs declared disc input or explicit host path |
| `capture` | Required `identity=LABEL` (1–160 bytes), mounted media; state input requires matching origin receipt |
| `schedule` | Required `identity` and `--input schedule=PATH`; initial `--disc` and named `--replacement` media |

Read/inspect actions export `cdrom.json` (`psx.runtime-cdrom/v2`), with
`file.bin` or `sectors.bin` for byte reads. Sector sizes follow the selected mode;
short reads fail. Command submission requires ready native queues and executes
guest MMIO stores. Response wait leaves FIFO unconsumed/unacknowledged.
`savestate` applies only to commands.

Capture bounds/defaults: `frames=1..36000` (60),
`events=1..65536` (65536), `bytes=1..8388608` (8388608), correlation
`mutations=1..65536` (4096), `reads=1..8388608` (8388608).
Optional aligned `stop` must arrive before frame exhaustion. Native capacity
floors also apply.

`audio=1` joins PCM with `samples=1..2646000` (441000), `blocks=1..65536`
(16384), `spuevents=1..65536` (16384). `decoded=1` requires audio and
`workers=1..65536` (65536); worker arguments otherwise refuse. Audio capacities
without audio refuse. Capture requires actual media ownership; unsupported
cached/transformed Filev2 owners refuse admission.

## Capture interpretation

The native envelope is version 1 and CD observation wire version 3. Reports use
`psx.runtime-cd-capture/v2`, `psx.runtime-cd-transactions/v2` and
`psx.native-history/v2`, with exact harness binary/source identity. Contexts are
384 bytes, queues 32 bytes, scheduler projections 48 bytes, worker rows 80 bytes
and worker status 48 bytes. Use native layouts and capacity floors.

The context exposes actual register bank, ReadingState/ReadingType, status,
speed/read span, mask, mode/filter, MSF positions, ATV, LocP/LocL and four queue
elements. Queue storage is initialized only on construction. `clear`, `getValue`
and `hitMax` retain native stored bytes/value behavior; only active payload prefix
bytes are qualified, with unused observation bytes zero. A defined raw value can
exist without `has_value`. Response insertion records actual dropped/head/tail/
promoted branches and the native return value, including false tail insertion.
Each response names its native stage: data-ready, complete, acknowledge, end or
error. Its command queue or stream identity and delivery `response_id` join
insertion to guest reads and acknowledgments. A dropped response still has a stage;
it does not establish guest delivery. FIFO command-validation errors retain their
submission queue without inventing a handler invocation.

Capture `complete` qualifies journal integrity. Verify the selected command's
terminal response and ordered guest-stored bytes separately. A first IRQ or a
handler returning without another continuation does not establish completion.
When reading stops, stream transitions retain pending CD schedules and their next
observed dispatch, replacement, cancellation or pending-at-end outcome. An
unresolved outcome remains unqualified.

The fork has 11 scheduler scale slots, with native CD projections for FIFO,
command continuation, read and CD DMA callbacks at slots 2, 3, 4 and 10. Pass snapshot and live
mask/target tests retain callback chronology even if another callback cancels a
slot after the pass selected it. Existing native targets and owning identifiers
are recorded; no replacement scheduler or guessed callback lineage is supplied.

FIFO mutation and consumed spans are native byte receipts. DMA reports actual
copied and dropped destination extents, including unmapped destinations and
scratchpad prefixes; they do not infer a full copy from requested byte count.
Media query/backend/image records expose native outcomes, owning file extents and
literal declared track identities. Backend zero fill, short/failure outcomes and
native CDDA/XA feed counts remain distinct observations.
CUE track views begin at INDEX 01 and exclude their preceding pregap from the
backed program extent. The full view must fit its file; no extent is silently
clamped. An invalid CUE rejects the mount instead of falling through to another
descriptor parser or a raw-image guess. Track lengths include pregap metadata;
the absolute program end and leadout subtract that pregap once. Position lookup
at or beyond leadout fails,
and guest GetlocP returns an error instead of its previous position bytes.
CDDA reads beyond a program extent synthesize silence without a backing read.
That outcome differs from a short backing read, which clears the destination
and fails. These rules do not establish hardware error-code fidelity.

Audio joined capture requires native Audio ABI 2 and the exact revision-2
14-slot settings environment described in [Audio](audio.md); incompatible setting
meanings reject.
Decoded capture also requires bounded worker capacity. Worker records carry batch,
sequence, epoch, last published CPU cycle, native predicates and publication
lineage. CPU handoff joins exact publication sequence ranges. These observations
establish native predicate and handoff order. They do not establish that a
captured XA sector caused the IRQ, DSP/hardware fidelity, worker race/deadlock
freedom or acceptable performance. Those require actual native missions and
measured controls.

Synthetic rows qualify parser contracts only. Native seed states use v6 and record
host edits. Preserve failed evidence; validate affected producer/worker/DMA behavior
under a verified build.

## Native calls

`PCSX.History.snapshot()` owns the frozen journal, payload, worker lane and joined
Audio events. Export and transaction analysis reuse that capture. Its `status`,
`records` and `bytes` functions retain the existing zero-based range conventions;
`correlateCD()` performs native worker chronology, publication and forward Audio
joins. `PCSX.History.correlateCD(capture?)` accepts the same snapshot. Acquisition
requires settled paused ownership and matching frozen epochs. Correlation refuses
incomplete evidence. Failed acquisition retains the raw frozen journal and an
incomplete transaction report; it does not clear recorder evidence.

`PCSX.CD.captureChecked()` returns owned controller fields after complete native
checked-state capture and validation, without decoding the full state in Lua.
`PCSX.CD.waitResponse({maxVsyncs=60}, callback)` counts native
Vsyncs, requests a pause and checks the response only after execution and event
dispatch unwind. It leaves the FIFO unconsumed and unacknowledged. Bounds are
1–120 Vsyncs and 64 pending handles. The callback receives
`(response, nil)` or `(nil, error)` once; response includes copied `state`,
`vsyncs`, `interrupt` and `command_error`. `handle.cancel()` requests completion
with an error. Completion, cancellation, reset, state load and shutdown release
listeners and callback references. Retain the handle when cancellation is needed.

`PCSX.CD.peekLid()` reads the current flag without applying scheduled closure.
`openLid()`, `closeLid()` and `scheduleCloseLid()` require checked admission;
scheduled closure uses the existing one-second guest-cycle target. `mount(path)`
prepares an owned image and checks media topology before replacing it; `eject()`
installs an owned failed image. Both require an open lid and idle controller
transfers. Pending commands/responses, unread or unrequested buffered data,
read/seek/play work and CD DMA refuse with `Busy`. Fully consumed cached data
does not count as pending transfer work. Failed preparation retains the old image. Successful mounting signals
`IsoMounted` after releasing the lease and never closes the lid or resets the
guest. A notification failure after replacement is `CommitFailed`; rollback is
not promised. The baked mission exposes these operations with actions `open-lid`,
`close-lid`, `schedule-close-lid`, `mount` and `eject`. Declare all replacement
media inputs explicitly; a host path change alone does not extend receipt provenance.

`iso:listDirectory(path, {maxEntries=4096})` traverses ancestors once and returns
owned rows `{name, lba, size, isDir}` plus the normalized path. `listDir` uses the
same native operation and retains its rows-only return value.
Paths are at most 4096 bytes, components at most 255 bytes, directory extents at
most 8 MiB and entry bounds 1–65536. Names are exact and case-sensitive; missing
directories, relative components, malformed records and exceeded bounds refuse.
The bound applies during native collection, before Lua table construction.

## Transfer bounds

For normal guest DMA, the shared DMA owner masks MADR to `0x7ffffc` before
calling the CD owner. A guest scratchpad-looking address can therefore select
RAM. Qualify the effective destination in the native start and copy receipts.
The CD owner uses BCR's low 16 bits times four; zero drains the available FIFO.
A transfer from an empty FIFO consumes and copies zero bytes. Nonempty draining
clears data readiness; DMA scheduler completion and CHCR clearing remain separate
observations. RAM wrapping can produce multiple copy receipts. Native memory-file
scratchpad prefixes and unmapped drops retain their own destination receipts;
a normal guest DMA run does not establish those paths.

## Scheduled replacement

`action=schedule` reads a data-only file:

```text
psx.disc-schedule/v1
1 open
1 eject
1 mount second
1 close
2 stop
```

Declare `--disc FIRST.cue --replacement second=SECOND.cue`. Frames are monotonic
GPU Vsync counts in `0..36000`; source order applies within a frame. The final
stop must have a positive frame. Files are at most 8 KiB and contain at most 32
operations. `mount` requires a declared name; other operations take no argument.
Native open-lid, idle-transfer and topology checks apply to every operation.

The mission pauses and settles at each scheduled boundary, freezes and exports
the preceding CD segment, performs checked host operations and starts a new
segment. All same-frame operations finish before restarting, so an ejected image
must be replaced within that group. At most eight segments are retained.
`events=1..65536` (8192) and `bytes=1..8388608` (1048576) apply per segment and
must fit the native boundary floor. Exported artifacts have `segment-NN-`
prefixes; transaction payload references use the corresponding prefixed file.
`disc-schedule.json` records ordered before/after controller and lid observations,
cycles, segment identities, stop reason and failures. Initial/final v6 states are
separate from recorder intervals. The total capture budget still applies.

Overflow or operation failure preserves exported prefixes and the failure report.
Replacement is a host operation between guest intervals, not a guest command.
Preparation failure retains the old image. After successful replacement, a
notification or host-file failure can leave changed state; no rollback is promised.
