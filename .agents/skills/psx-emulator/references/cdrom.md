# CD-ROM observation

Use `bin/harness runtime run --mission cdrom` for serialized inspection, command
submission or bounded native controller capture. Read [runtime](runtime.md)
first. A capture requires actual mounted media ownership and bounded native
capacity. Unsupported cached or transformed Filev2 owners refuse admission;
normal cached gameplay remains unchanged.

The native envelope is version 1 and CD observation wire version 3. Reports use
`psx.runtime-cd-capture/v2`, `psx.runtime-cd-transactions/v2` and
`psx.native-history/v2`, with exact harness binary/source identity. Contexts are
384 bytes, queues 32 bytes, scheduler projections 48 bytes, worker rows 80 bytes
and worker status 48 bytes. Native limits own event/payload/worker budgets and
minimum boundary capacity. Do not reuse old word offsets or old controller names.

The context exposes actual register bank, ReadingState/ReadingType, status,
speed/read span, mask, mode/filter, MSF positions, ATV, LocP/LocL and four queue
elements. Queue storage is initialized only on construction. `clear`, `getValue`
and `hitMax` retain native stored bytes/value behavior; only active payload prefix
bytes are qualified, with unused observation bytes zero. A defined raw value can
exist without `has_value`. Response insertion records actual dropped/head/tail/
promoted branches and the native return value, including false tail insertion.

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

Audio joined capture requires native Audio ABI 2 and the exact revision-2
14-slot settings environment described in [Audio](audio.md); legacy setting
meanings reject. The CD envelope, context wire version 3, eleven scheduler scales
and offsets remain unchanged.
Decoded capture also requires bounded worker capacity. Worker records carry batch,
sequence, epoch, last published CPU cycle, native predicates and publication
lineage. CPU handoff joins exact publication sequence ranges. These observations
do not establish DSP/hardware fidelity, worker race/deadlock freedom or acceptable
performance; those require actual native missions and measured controls.

Synthetic rows qualify parser contracts only. Native seed states use the current
v6 schema and record host edits. Preserve failed captures and run affected
producer/worker/DMA missions under a verified fork build before claiming behavior.

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
`PCSX.CD.waitResponse({maxVsyncs=60, timeoutMs=10000}, callback)` counts native
Vsyncs, requests a pause and checks the response only after execution and event
dispatch unwind. It leaves the FIFO unconsumed and unacknowledged. Bounds are
1–120 Vsyncs, 1–600000 milliseconds and 64 pending handles. The callback receives
`(response, nil)` or `(nil, error)` once; response includes copied `state`,
`vsyncs`, `interrupt` and `command_error`. `handle.cancel()` requests completion
with an error. Deadline, reset, state load and shutdown release listeners and
callback references. Retain the handle when cancellation is needed.

`PCSX.CD.peekLid()` reads the current flag without applying scheduled closure.
`openLid()`, `closeLid()` and `scheduleCloseLid()` require checked admission;
scheduled closure uses the existing one-second guest-cycle target. `mount(path)`
prepares an owned image and checks media topology before replacing it; `eject()`
installs an owned failed image. Both require an open lid and idle controller
transfers. Failed preparation retains the old image. Successful mounting signals
`IsoMounted` after releasing the lease and never closes the lid or resets the
guest. A notification failure after replacement is `CommitFailed`; rollback is
not promised. The baked mission exposes these operations with actions `open-lid`,
`close-lid`, `schedule-close-lid`, `mount` and `eject`. Declare all replacement
media inputs explicitly; a host path change alone does not extend receipt provenance.

`iso:listDirectory(path, {maxEntries=4096})` traverses ancestors once and returns
owned rows `{name, lba, size, isDir}` plus the normalized path. `listDir` aliases it.
Paths are at most 4096 bytes, components at most 255 bytes, directory extents at
most 8 MiB and entry bounds 1–65536. Names are exact and case-sensitive; missing
directories, relative components, malformed records and exceeded bounds refuse.
The bound applies during native collection, before Lua table construction.
