# Frames, input and display

Run `--mission frames` ([source](../../../../src/lua/missions/frames.lua)) for a
counted Vsync interval with optional digital input/display capture. Use
[Replay](replay.md) for schedules, comparisons or terminal PC/register/RAM conditions.

## Procedure

Select target, EXE entry or state; `frames=1..36000` (1).
`buttons=START,CROSS` holds case-sensitive `PCSX.CONSTS.PAD.BUTTON` names on
`slot=1|2` (1), pad 1. `screenshot=1` requests pixels; `savestate=1` retains state.
Mission pauses at final Vsync, exports RAM/registers and `frames.json` with
before/after cycles and optional state.

## Application

Frames count native GPU Vsync events, not UI frames, instructions or fixed time.
Pressed states are reported before overrides clear. Digital overrides neither
isolate physical input nor implement analog trajectories; process exits afterward.

`screen.bin` holds pixels; `screen.json` records size/format/availability.
Nonempty displays also yield P6 RGB `screen.ppm`. Native row order remains;
pixels are little-endian BGR555 (16-bit) or RGB24. `PCSX.convertScreenshot` converts pixels;
Lua writes the PPM header/file. Uninitialized display: unavailable, zero bytes,
no PPM. Capture covers displayed buffer, not all VRAM or GPU history.
