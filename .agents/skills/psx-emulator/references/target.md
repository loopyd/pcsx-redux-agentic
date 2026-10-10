# Boundaries and captures

Run `--mission target` ([source](../../../../src/lua/missions/target.lua)) to stop
and capture a machine boundary.

## Procedure

Set aligned U32 `target=ADDRESS`, or use the supplied EXE's header entry.
Restored state can be captured immediately; see [restoration](states.md#restore-and-branch-an-experiment).
An unreached target times out; repeated targets can be captured on a later visit.

## Application

Pause precedes the target instruction. Check receipt, `ram.bin` (2 MiB),
`registers.json` (32 GPRs, PC, cycles; `psx.runtime-registers/v1`) and optional
`state.pbuf` (`savestate=1`). RAM/registers omit devices, delayed loads and other
CPU state. Mission captures and exits; no persistent interactive session.
