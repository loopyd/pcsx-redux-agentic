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

Audio joined capture requires the native audio recorder and exact environment.
Decoded capture also requires bounded worker capacity. Worker records carry batch,
sequence, epoch, last published CPU cycle, native predicates and publication
lineage. CPU handoff joins exact publication sequence ranges. These observations
do not establish DSP/hardware fidelity, worker race/deadlock freedom or acceptable
performance; those require actual native missions and measured controls.

The existing 31 fixture sources cover parsing, accounting, refusals and seven
native seed missions. Synthetic rows qualify parser contracts only. Native seed
states use the current v6 schema and explicitly record host edits; changed fork
semantic domains cannot be given invented legacy meanings. Preserve failures and
run the seven producer/worker/DMA missions separately under a verified fork build.
