# Debugger observations

Run `--mission watch` ([source](../../../../src/lua/missions/watch.lua)) for
selected Exec/Read/Write hits or a finite Exec boundary window.

## Procedure

Select target, EXE entry or restored state.
Set `address=U32`, `width=1..65536` (1), `kind=Exec|Read|Write` (Write),
and `hits=1..4096` (1). The action stops at that count and emits `watch.json`,
RAM/registers and optional `savestate=1`. Without an instruction window, an
unreached hit keeps the mission running.

Add `instructions=1..100000000` for a finite first-reach Exec window. This requires
`kind=Exec`, `width=1`, `hits=1` and a word-aligned address in canonical first-2MiB
RAM, `0x00000000..0x001ffffc`, `0x80000000..0x801ffffc` or
`0xa0000000..0xa01ffffc`. Without `instructions`, hit-count mode applies.
Acquisition/restoration is outside the finite window.

Initial snapshot follows pause and acquisition-callback batch completion. Exact
initial raw PC equality succeeds at step 0 with count 0 and no
native selected event. An explicit target/EXE entry must also equal the initial
raw PC. Otherwise two native Exec hooks observe subsequent boundaries. Only the
full-range hook increments `observed_exec_boundaries`; there is no PC deduplication.
At a selected hit or boundary N, the action pauses synchronously and resolves
after the callback batch. A hit exactly at N succeeds under either hook order;
otherwise reaching N fails. The action never resumes from this terminal pause.

## Application

Verify debugger address/alias mapping and receipt. Events retain actual address,
width, cause and registers **before** the selected instruction/access. They do
not prove retirement or capture completed write values; a range watch is not a
complete DMA/device/bus trace.

Hooks: false removes, true keeps; retain/protect callbacks and pause for capture.
Interpret observation boundary before comparison.

Bounded reports retain requested address, raw callback address/register PC,
`initial`, `stop`, `first_reach` on success, `last_boundary`, pause context, limit,
count and reason. A later hit requires matching selected/range address, width,
cause, raw PC and cycles. Native matching owns alias normalization; a selected
native alias with a different raw address or register PC fails.

`observed_exec_boundaries` counts debugger observations after step 0, not retired
instructions. Delay-slot work follows native interpreter/debugger processing;
`updatedPC` can add observations without retirement. Width 1 is the selected
watch interval, not instruction size. The sole qualified selected-word source is
the complete 2097152-byte successful `ram.bin` plus `first_reach.raw_pc` and original
loading/address metadata. Independently resolve the physical offset, read the
complete word and compare original bytes; this is RAM evidence, not a fetched or
instruction-cache word. Validate receipt hashes before interpretation.

Keep interpreter/debugger mode fixed, host Lua JIT disabled and host register,
memory/state edits absent. Unexpected armed Pause/Run, exception pause, reset,
state load, memory LUT changes or callback errors fail even beside a hit. Silent
debugger/CPU-mode or direct host changes are not fully observable through Lua;
they invalidate the observation and are not certified absent by a passed receipt.

Only an exact positive observation captures RAM/registers and optional state.
Bound, alias and interruption failures retain raw `watch.json` before failed
exit. Capture errors become failures before the report; partial files carry no
qualified capture claim. Report/I/O failures may prevent that report. Harness
termination retains failed receipt/logs, but final count and a completed capture
may be unavailable. Check the actual receipt; no failure establishes first reach.
