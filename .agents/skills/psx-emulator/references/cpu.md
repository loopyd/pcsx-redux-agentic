# CPU registers

Run `--mission registers` ([source](../../../../src/lua/missions/registers.lua))
to inspect CPU banks or edit GPR/PC at a stopped context.

## Procedure

Omit edit arguments to inspect. `register=1..31` plus `value=U32` edits one GPR;
`pc=ADDRESS` edits aligned PC. Register zero refuses edits; other banks are
inspection-only. `savestate=1` retains the result.

## Application

Check `cpu.json` before/after GPR, PC, HI/LO, CP0, CP2D and CP2C.
Host edits change visible storage, not pending-load/delay machinery; select a
suitable context. Use [GTE](gte.md) for COP2 edits or native commands.

For step/call observations, `execution.capture()` wraps `PCSX.captureCPUChecked()`:
full checked-state admission, copied CPU/delay/exception fields and safe memory
words, without full-state Lua decoding. Bounds/errors: [API](api.md#cpu-observation).
