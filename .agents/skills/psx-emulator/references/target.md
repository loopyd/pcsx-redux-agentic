# Boundaries and captures

Run `--mission target` ([source](../../../../src/lua/missions/target.lua)) to stop
and capture a machine boundary.

## Procedure

Set aligned U32 `target=ADDRESS`, or use the supplied EXE's header entry.
An explicit target equal to the current raw PC is captured after pause settles,
including the fresh reset PC. An implicit EXE entry waits for the loader to reach
that entry. Restored state can be captured immediately; see
[restoration](states.md#restore-and-branch-an-experiment).
Repeated targets can be captured on a later visit.

Set `buttons=START,CROSS` to hold case-sensitive `PCSX.CONSTS.PAD.BUTTON` names
while acquiring the target, on `slot=1|2` (1), pad 1. Names must be distinct.
Overrides apply after restoration and breakpoint setup, before execution resumes.
An immediately captured current target applies no input. Overrides clear before
capture. Physical input remains active; digital overrides supply no analog input.

`frames=1..36000` limits acquisition to counted GPU Vsync events. With buttons,
it defaults to 36000; otherwise omitting it leaves acquisition unbounded.
Exhaustion fails without a qualified capture. Vsyncs measure guest events, not
instructions or elapsed time.

## Application

Pause precedes the target instruction. Check receipt, `ram.bin` (2 MiB),
`registers.json` (32 GPRs, PC, cycles; `psx.runtime-registers/v1`) and optional
`state.pbuf` (`savestate=1`). RAM/registers omit devices, delayed loads and other
CPU state. Mission captures and exits; no persistent interactive session.
Bounded acquisition also emits `target.json` (`psx.runtime-target/v1`) with the
observed Vsync count, selected slot, requested buttons and captured registers.
