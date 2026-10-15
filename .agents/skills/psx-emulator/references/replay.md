# Repeated continuation

Run `--mission replay` ([source](../../../../src/lua/missions/replay.lua)) for
bounded scheduled state continuations; use [comparison semantics](states.md#structured-comparison).
Measures selected observations' repeatability, not full determinism or hardware accuracy.

## Procedure

Supply successful `--origin`, matching BIOS/disc, `--input state=PATH` and
`--input schedule=PATH`; omit EXE. Set `runs=2..8` (2), comparison `depth=0..64`
(16), per-checkpoint `limit=1..65536` (1024). Inspect receipt and `replay.json`; a successful run can report `equal:false`.

<a id="schedule-format"></a>

Schedules are data-only UTF-8/ASCII, with CRLF/blank lines allowed:

```text
psx.schedule/v1
0 analog 1 analog RX:128,RY:128,LX:0,LY:128
0 sample registers
1 analog 1 analog LX:255
1 buttons 1 START
2 sample registers
2 buttons 1 -
2 analog 1 analog LX:128
3 stop
```

| Command | Meaning |
| --- | --- |
| `FRAME sample PATH` | Compare subtree; `.` selects the whole state |
| `FRAME screen` | Compare display metadata/pixels |
| `FRAME buttons SLOT NAMES` | Replace held buttons on slot 1/2; comma-separated native names or `-` to release |
| `FRAME analog SLOT MODE AXES` | Replace analog input on slot 1/2. Modes and named axes follow [Frames](frames.md). `FRAME analog SLOT -` releases it. |
| `FRAME stop` | Required final command; stop at frame 1–36000 |
| `FRAME stop CONDITION` | Poll after earlier commands; frame is deadline |

Frames count GPU Vsyncs after restoration; frame 0 runs paused. Use nondecreasing
frames 0–36000; same-frame commands retain order. Limits: 512 commands, four
state/display checkpoints, 64 KiB schedule. Unknown commands, buttons, axes or
modes reject. Invalid ranges, duplicates, missing checkpoints or stop, invalid
paths, incompatible boundaries and post-stop commands also reject.

Conditions: `pc VALUE`, `register INDEX VALUE` (GPR 0–31), or `memory OFFSET HEX`
(physical 2 MiB RAM, 1–64 byte pairs). Integers accept decimal/hex; PC must align;
Lua expressions reject. Poll each paused Vsync after earlier commands, from frame
1 onward. Matches retain observation/frame/cycles; unmet deadlines fail with
`condition.json`. Intermediate instructions/MMIO are unobserved.

`--input alternate=PATH` replaces the primary schedule for runs 2 onward.
Checkpoint frame/type/path sequence and stop deadline must match. Buttons,
analog inputs and conditions may differ. Otherwise all runs use the primary
schedule. Overrides clear between runs and after final stop or guarded failure.
`input-cleanup.json` records the final released state. Analog overrides isolate
physical input on their slot and restore the prior native device type and mode,
with axes at neutral 128. Physical input remains active on other slots and when
only button overrides are used.

## Application

`psx.runtime-replay/v1` compares exact start/stop/checkpoint cycles, selected state
and observed buttons, even unpolled buttons. Active analog overrides also compare
axes, mode and the latest native serial reply at each checkpoint. The cumulative
native poll counter is outside saved state and is excluded from equality.
Each run's `inputs` records analog commands in source order, with frame, slot,
mode, axes and the latest native poll observation. A compared difference sets
`equal:false`. The first divergent boundary retains paths and stop reasons.

| Capture | Meaning |
| --- | --- |
| `baseline-NNN.pbuf` | Baseline full state |
| `baseline-NNN.bin` | Baseline display bytes; metadata in report |
| `divergence.pbuf` | First differing state |
| `divergence.bin`, `divergence.json` | First display difference |
| `state.proto` | Running-build schema |

Captures cap at 120 MiB within the harness's 128 MiB total. Receipts bind state,
schedules, scripts, tools and media. Observations follow paused Vsync dispatch,
not instruction/audio-sample boundaries; [Runtime](runtime.md#evidence-contract)
defines unpinned settings and host effects.

Equal subtrees prove neither whole-state nor execution equality. Hidden device
state, host audio queues and PCM are unexamined. Pad overrides
and native poll counters are host controls outside saved state. Native serial
replies record bytes returned by the pad owner, including partial packets. They
do not prove guest UART reads. Check packet completion and compare guest serial
buffers or RAM at the selected checkpoints. `screen` compares storage/ranges
and dimensions/format/availability separately; equal unavailable displays remain
`image_comparison_available:false`, not image evidence.

Preserve deferred Vsync boundaries through `PCSX.nextPause`. Start the next
restoration only after CPU execution and event dispatch have unwound.
