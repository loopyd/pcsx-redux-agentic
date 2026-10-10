# Boundaries and captures

## Purpose

Use [target.lua](../../../../src/lua/missions/target.lua) to stop and capture a machine boundary.

## Procedure

Follow [Runtime](runtime.md#invocation); Canonical `support` and restoration helper `snapshot` are preloaded. Set aligned U32 `target=ADDRESS`, or use the supplied EXE's
header entry. A restored state can be captured immediately
([restoration](states.md#restore-and-branch-an-experiment)).
An already-passed target times out; use finite process bounds.

## Application

The action pauses before the target instruction, captures and exits.
Check receipt and `ram.bin` (2 MiB), `registers.json` (32 GPRs, PC, cycles;
`psx.runtime-registers/v1`), plus raw `state.pbuf` with `savestate=1`.
RAM/registers alone omit devices, delayed loads and other CPU state. This is a
bounded capture, not a persistent interactive session.
