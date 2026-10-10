# CPU registers

## Purpose

Use [registers.lua](../../../src/lua/missions/registers.lua) to inspect CPU banks or edit GPR/PC
at a stopped context. Follow [Runtime](runtime.md#invocation).

## Procedure

Declare `support`; restoration also requires `snapshot`. Omit edit arguments
to inspect. `register=1..31` plus `value=U32` edits one GPR;
`pc=ADDRESS` edits aligned PC. Register zero rejects edits; other banks are
inspection-only. `savestate=1` retains the result.

## Application

Check `cpu.json` before/after GPR, PC, HI/LO, CP0, CP2D and CP2C values.
Edits change visible storage, not pending-load/delay machinery, so select a
suitable context. They are host changes, not guest execution.
Use [GTE](gte.md) for COP2 edits or native commands.

For stepping and call-boundary observations, `execution.capture()` wraps the native
`PCSX.captureCPUChecked()` API. It retains full checked-state admission and returns
copied CPU, delay, exception, and safe instruction-memory fields without decoding
the complete state in Lua. See [API](api.md).
