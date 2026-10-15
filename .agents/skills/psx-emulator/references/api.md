# Extending emulator actions

Extend Linux missions with pinned source and bundled LuaJIT, `pb` and `protoc`.
Prefer existing bindings; do not add an external decoder or generated Rust runner.

## Procedure

1. Check [action routing](../SKILL.md#select-an-action) for an existing runner.
2. Locate the owner below; establish timing, side effects, threads and exclusions.
3. Require `support` before native calls; retain callbacks and protect completion/
   failure. Follow [Runtime](runtime.md#invocation) for host JIT, preloading and inputs.
4. Validate a real mission and bounded failure; update its procedure. Missing
   bindings require [native customization](native.md), specific source
   authorization and independent semantic review.

Fork-owned references and pinned source govern routine work. Consult external
library docs for scoped research, verify against pinned source, and retain reusable
findings here.

| Reference | Bundled calling rule |
| --- | --- |
| [Core Lua API](../../../../src/core/pcsxffi.lua) | Raw state/schema, controller overrides and display capture; GUI states use gzip |
| [Breakpoints](watch.md) | Interpreter/debugger observations; retain hooks, guard errors, pause explicitly; false removes a hook |
| [Memory/registers](memory.md) | Bounds-check pointers; invalidate code caches; verify MMIO effects in source |
| [Events](../../../../src/core/eventslua.cc) | Retain listeners; count native Vsyncs; defer host work outside dispatch |
| [Files](../../../../src/support/file.h) | Native slices and explicit state inputs; close files |
| [States](states.md) / [bundled lua-protobuf](../../../../third_party/lua-protobuf) | Schema reflection and `int64_as_string` (`#decimal`); verify bundled behavior |
| [Runtime invocation](runtime.md#invocation) | Portable settings and explicit interpreter/debugger options |
| [LuaJIT controls](https://luajit.org/ext_jit.html) / [FFI callbacks](https://luajit.org/ext_ffi_semantics.html#callback) | `jit.off()` stops host compilation; `jit.flush()` clears traces. Native event dispatch may synchronously reenter Lua; do not rely on automatic call blacklisting |

Source paths relative to fork root:

| Owner | Inspect for |
| --- | --- |
| `src/core/pcsxffi.lua`, `pcsxlua.cc` | Pointers, registers, state/schema, screenshots and callbacks |
| `src/core/sstate.h`, `sstate.cc`; `src/spu/types.h`, `freeze.cc` | Fork-owned v6 schema, fixed regions, delayed loads, restoration and serialized DSP coverage |
| `src/core/debug.cc`, `psxinterpreter.cc` | Pre-instruction/access observations, retirement and exception behavior |
| `src/core/eventslua.cc`, `pad.cc` | Vsync and controller overrides |
| `src/core/ui.cc`, `src/main/main.cc`, `src/core/arguments.cc` | EXE handoff, callback dispatch, CLI ordering and settings |
| `src/core/psxmem.cc` | Debugger memory-file versus guest bus effects |
| `src/gpu/soft/gpu.cc` | GP0/GP1, VRAM, display rectangle, rows and pixel formats |
| `src/core/gte-instructions.cc` | Native COP2 arithmetic; interpreter gates on Status.CU2 |
| `src/spu/registers.cc` | Port side effects |
| `src/core/isoffi.lua`, `cdrom.cc` | Mounted media/sector modes/ISO reader versus controller lifecycle |
| `src/core/cdlua.cc`, `historylua.cc` | Checked CD observations/waits, prepared media changes and frozen native joins |
| `src/core/memorycardlua.cc`, `memorycard.cc`, `sio.cc` | Owned card images, shared directory/chain parsing, staged edits and persistence |
| `third_party/lua-protobuf/README.md` | Reflection, integer options and decoding behavior |

## JSON

`PCSX.encodeJSON(value)` encodes nil, booleans, finite numbers, byte strings,
arrays and string-keyed objects. Numbers use `%.17g`; object keys sort by bytes.
Control bytes, quotes and backslashes use lowercase `\u00xx` escapes. Other bytes
remain unchanged. Calls reject cycles, unsupported values, malformed arrays,
nesting beyond 64 levels, more than one million values/keys, and output over 64 MiB.
`PCSX.jsonArray(table?)` tags a contiguous numeric-key table; nil creates an empty
array. `support.json` and `support.array` alias these bindings.

## GPU decoding

`PCSX.decodeGPU(bytes, port, wordLimit, packetLimit)` parses an immutable GP0 or
GP1 stream into the existing packet tables. `PCSX.readGPUChain(ram, address,
wordLimit, nodeLimit)` gathers an immutable physical 2 MiB ordering table. The
`packets` wrappers supply defaults and adapt options. Both calls retain the
[GPU](gpu.md) bounds, operands and refusals; neither runs renderer commands.

## State wire selection

`PCSX.validateSaveStateWire(bytes)` checks up to 64 MiB against the native state
schema without decoding or loading a state. It rejects unknown fields, duplicate
singular fields, incompatible wire types, truncation, invalid tags/scalars,
more than 100000 tags/scalars and nesting beyond 64 levels. Strict `snapshot`
decoding uses this call before bundled `pb.decode`. Version and checked-restore
validation remain separate; valid wire bytes alone do not authorize restoration.

`PCSX.selectSaveStateWire(bytes, path, strict?)` validates the whole input and returns
the selected original field occurrences in a parent-message envelope. Its metadata
includes `wire`, `version_wire`, `parent_type`, `field_name`, `kind`, `repeated`,
`indexed` and `parent_present`. `snapshot.decodeSelected` delegates scalar/default
decoding to bundled `pb`, preserving exact integers and encoded presence. Dotted
paths use one-based indices up to one million. Empty path selects the root. Strict
mode uses the full validator above; query mode skips unknown fields and retains
the last singular occurrence while rejecting malformed known fields anywhere.
The same size, value-count and nesting bounds apply. Full checked restore admission
remains separate.

## Guest store probes

`PCSX.packMMIOProbe(scratch, width, operations)` returns an immutable little-endian
MIPS store program from a contiguous array of `{address, value}` U32 pairs.
Widths are 8, 16 or 32 bits; 1–262000 stores consume `52 + 8*N` bytes. Scratch is
aligned RAM in segments 0, 4 or 5 and the complete program must fit physical 2 MiB.
`PCSX.packVRAMUploadProbe(scratch, x, y, width, height, pixels)` builds the same
program directly from exact BGR555 rectangle bytes, including GPU FIFO reset and
zero padding for an odd final pixel. Both only pack bytes. The `bus` executor
retains checked admission, guest stores, and scratch/register restoration.

## CPU observation

`PCSX.captureCPUChecked()` returns copied PC, GPR[34], CP0[32], decimal cycles,
last executed word, nonempty delay slots, ISR flags, exception context, and nearby memory
words. Calls retain full checked-state refusals and return the same `savestate:`
errors as checked saves. Reads are aligned U32 words in segments 0, 4, and 5 from
the first 2 MiB of RAM, BIOS, or scratch; unavailable words are omitted, while
zero words remain present. Tables own their values. `execution.capture()` uses
this call and marks its nearby list for JSON array encoding.

## Screenshot conversion

`PCSX.convertScreenshot(bytes, width, height, bpp)` returns packed RGB24 without
reading live GPU state. Dimensions are integral `0..1024` by `0..512`; `bpp` is
native screenshot enum `0` (little-endian BGR555) or `1` (RGB24). Storage must fit
exactly. Zero-area inputs return an empty string; RGB24 returns the input unchanged.
BGR555 keeps row order, ignores bit 15 and expands each channel with
`(component * 255 + 15) / 31`. Raw screenshot capture and metadata remain separate.

`MemoryAsFile::writeBlock` bypasses device handlers. [MMIO probes](bus.md) execute
effects using scratch RAM and advance guest/device time.

## Deferred pause work

`PCSX.nextPause(function)` queues up to 64 callbacks for a paused outer-loop
boundary, after CPU execution and event dispatch unwind and pending loads finish.
Native dispatch preserves FIFO order, rechecks the boundary before each callback,
and retains nested callbacks for a later tick. It does not pause or step the guest;
call `PCSX.pauseEmulator()` when a pause is needed. Checked operations still apply
their own recorder, GPU, and state admission checks.

`PCSX.nextTick` remains general UI polling work and can run inside CPU execution.
Use `nextPause` for checked capture after a breakpoint or device callback. Follow
[Step](step.md), [Trace](trace.md), and [Replay](replay.md) boundaries. GPU,
History, Audio and CD finalizers also queue native pause callbacks.

## Controller overrides

Each `PCSX.SIO0.slots[1|2].pads[1]` exposes
`setInputOverride(mode, RX, RY, LX, LY)`, `clearInputOverride()` and
`getInputState()`. Modes are `analog` or `digital`. Axes are integral 0..255
with neutral 128. Digital mode requires four neutral axes. Validation precedes
mutation. The override connects the selected native analog pad and excludes
keyboard, SDL and shared-display buttons and axes. Existing button overrides
still apply. Clearing restores the previous device type and analog mode and
neutralizes axes. Settings and other slots are unchanged.

`getInputState()` copies active ownership, isolation, mode, native device type,
axes, configuration mode, decimal cumulative `poll_count`, `serial_command`,
`serial_complete` and up to nine raw `serial` bytes from the latest poll.
`timeline.inputState` converts those bytes to `serial_hex`. The prefix starts
with the pad-select reply and retains only this poll. Mode changes affect the
next read packet. Guest configuration commands can still change protocol state.
Serial observations describe native pad replies, not guest UART consumption.
Overrides and poll observations are not serialized in checked states.

Maintained callers register cleanup with `support.onCleanup(callback)` before
applying overrides. `support.finish` and guarded failures drain callbacks once
in reverse order. Cleanup failures retain the original mission error and report
the cleanup error separately. [Frames](frames.md) and [Replay](replay.md) own
argument, schedule, observation and cleanup procedures.

## Device APIs and limits

[History](history.md), [Audio](audio.md), [CD-ROM](cdrom.md) and
[Memory cards](memcard.md) own their native signatures, capacities and failure rules.

Save/load can alter scheduling; serialized state does not establish determinism
or complete hardware coverage. `PCSX.SPU.playAudio` provides host preview, not
emulated PCM. Analog trajectories and hardware-fidelity claims
require explicit inputs, bounds and validation; never approximate silently.
Keep experiment evidence outside procedural references.
