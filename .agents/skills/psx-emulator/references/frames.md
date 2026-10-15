# Frames, input and display

Run `--mission frames` ([source](../../../../src/lua/missions/frames.lua)) for a
counted Vsync interval with optional controller input and display capture. Use
[Replay](replay.md) for schedules, comparisons or terminal PC/register/RAM conditions.

## Procedure

Select target, EXE entry or state; `frames=1..36000` (1).
`buttons=START,CROSS` holds case-sensitive `PCSX.CONSTS.PAD.BUTTON` names on
`slot=1|2` (1), pad 1. `screenshot=1` requests pixels; `savestate=1` retains state.
Mission pauses at final Vsync, exports RAM/registers and `frames.json` with
before/after cycles and optional state.

`mode=analog` supplies real analog bytes with
`axes=RX:255,RY:128,LX:0,LY:128`. Axis names are case-sensitive, values are
integers in 0..255, and omitted axes default to neutral 128. `mode=digital`
selects the analog controller's digital mode and requires neutral axes.
Unknown axes, duplicate axes, unsupported modes and invalid values refuse before
execution. Axes require an explicit mode. Both modes isolate keyboard, SDL and
shared-display input on the selected slot while the override is active.

## Application

Frames count native GPU Vsync events, not UI frames, instructions or fixed time.
`frames.json` records buttons and native input before release.
`input-cleanup.json` records the released state after success or guarded failure.
Cleanup clears owned buttons, restores the prior native device type and analog
mode, and sets owned axes to 128. Buttons alone leave physical input active.

Native input reports include the latest poll count, returned serial bytes and
packet completion. A latest packet can be partial or precede an input change.
Join it to guest RAM or SIO observations before claiming that the guest received
an axis value. The override affects subsequent pad polls. It does not rewrite an
in-flight reply or isolate host audio, timing or other devices.

`screen.bin` holds pixels; `screen.json` records size/format/availability.
Nonempty displays also yield P6 RGB `screen.ppm`. Native row order remains;
pixels are little-endian BGR555 (16-bit) or RGB24. `PCSX.convertScreenshot` converts pixels;
Lua writes the PPM header/file. Uninitialized display: unavailable, zero bytes,
no PPM. Capture covers displayed buffer, not all VRAM or GPU history.
