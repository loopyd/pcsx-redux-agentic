# Native debugger stepping

Run `--mission step` ([source](../../../../src/lua/missions/step.lua)) for bounded
native step-into pause transitions. Step-over/out unsupported.

## Procedure

Choose target, EXE entry or state.
Required `identity=LABEL` (1–160 bytes) labels executable/overlay context;
receipt identifies bytes.

`steps=1..1024` (1) bounds transitions after target acquisition.
Optional aligned `stop=ADDRESS` succeeds before that instruction; exhausting
the step count first fails. Without stop, reaching the count succeeds.

`exceptions=stop|continue` defaults to stop on native exception pause or changed
ISR/EPC/Cause; an exception before the requested address fails. CP0 writes can
also change context, so this is not independent exception decoding.
`savestate=1` retains final state, including handled failure.

## Application

`steps.ndjson` flushes initial/transition PC, exact cycles, GPR/HI/LO, CP0, deltas,
pending loads/branches, ISR, Cause/EPC/Status/BadVAddr and nearby words. BadVAddr
may be stale; `next_is_delay_slot` describes the next instruction; absent delay
submessages use protobuf defaults. Nearby RAM/ROM/scratch reads have no side
effects but may differ from instruction cache. `last_executed_word` is serialized
CPU state; MMIO/unsupported aliases are not dereferenced.

`step.json` (`psx.runtime-step/v1`) reports reason, success, bounds, final context
and [coverage](trace.md). Journal overflow beyond 16 MiB fails. Handled errors
retain reports/failing receipts; aborts may leave only flushed records and logs.
Missing completion is never success.

Exceptions or breakpoints can interrupt a request; IRQs can cross instructions.
One row/request does not imply one retired instruction; use the boundary and
coverage limits above when interpreting results.
