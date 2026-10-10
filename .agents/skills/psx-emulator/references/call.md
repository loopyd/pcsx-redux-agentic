# Function calls

Run `--mission call` ([source](../../../../src/lua/missions/call.lua)) for a
host-directed function call with optional selected-PC observations.

## Procedure

Choose target, EXE entry or restored state. Set aligned `function`; aligned
`return` defaults to `0x80010000` and must differ from function/context entry.
`a0`–`a3` default to zero. Mission sets PC, RA, A0–A3, preserves other state,
and captures before the return instruction.

Optional `trace=ADDRESS` records registers/cycles on visits; `trace_limit=1..65536`
(4096). Overflow fails. `savestate=1` retains the result.

## Application

Verify receipt, capture boundary and `trace.json` (`psx.runtime-trace/v1`).
Host-established calls prove neither natural startup nor ABI validity. Selected-PC
observations are not a retired stream; interpret recursion/exceptions explicitly.
