# PCSX-Redux Agentic

PCSX-Redux Agentic is a Linux fork of [PCSX-Redux](https://github.com/grumpycoders/pcsx-redux)
for agentic coding, PlayStation reverse engineering, and behavioral analysis. It
combines the emulator and debugger with bounded native capture APIs and an embedded
Lua mission library. The interface and built-in messages use English (`en_US`) only;
translation catalogues, language selection, and the Lua `t_` translation helper are
removed. Existing saved `Locale` keys are ignored and omitted when settings are
saved. Japanese font support remains for PlayStation memory-card titles.

The upstream authorship and licenses remain in [AUTHORS](AUTHORS),
[LICENSE](LICENSE), and [LICENSES.md](LICENSES.md).

## Build locally

Clone the fork with its pinned submodules:

```sh
git clone --recursive https://github.com/loopyd/pcsx-redux-agentic.git
make -C pcsx-redux-agentic -j6 BUILD=Release
```

The supported host build is native Linux Make with a C++23 compiler. Docker,
Visual Studio, VS Code configuration, and CodeRabbit integration are removed.
Required pkg-config packages are `capstone`, `freetype2`, `libavcodec`, `libavformat`,
`libavutil`, `libswresample`, `libcurl`, `libuv`, `sdl3`, and `zlib`; the GUI also
links OpenGL, X11, and XCB. Other libraries come from the pinned submodules or
compatible system packages selected by the Makefile. Install dependencies through
your project's approved toolchain process. The fork does not publish cloud builds
or maintain hosted CI. The Linux host graph preserves x86-64 and AArch64 code paths;
validation on one architecture does not establish validation on the other.

`make runtests` runs existing host tests. `make tools` builds the PSX conversion
utilities. OpenBIOS and other guest programs stay in the pinned `src/mips`
submodule and use their own MIPS toolchain, for example:

```sh
make -C src/mips/openbios
```

Pinned upstream submodules retain their own build scripts and portability code.
Those scripts are not supported host build routes in this fork.

Linux Nix packaging remains in `flake.nix` and `pcsx-redux.nix`. Local AppImage
packaging uses `make appimage` and requires the corresponding Linux packaging tools.
These are separate from the native Make build.

## Embedded missions

The binary embeds 19 entry missions and 34 shared modules. Query their names,
roles, compiled Lua, and native state schema without starting emulation:

```sh
./pcsx-redux -mission-catalog
```

Select a builtin with `-mission NAME`, or trusted external Lua with `-dofile FILE`.
Both routes preload the embedded shared modules; no loose canonical Lua files
are required at runtime. Lua runs as host code without a sandbox.

Host integrations supply per-run settings, staged inputs and arguments, process
bounds, cleanup, and receipt validation. Supply your own licensed BIOS,
executables, and disc images, and keep proprietary inputs and captures local.
The [psx-emulator skill](.agents/skills/psx-emulator/SKILL.md) documents mission
selection, captures, and native APIs. Harness build and invocation instructions
belong in the integrating repository.

## Native APIs

Use native bulk operations for state capture, memory-card readback, byte-range
comparison, VRAM, screenshot conversion, JSON encoding, GPU packet decoding,
strict state wire validation, recorder batches, PCM streaming, upload probe packing,
and selective saved-state decoding. Keep Lua responsible for mission composition
and provenance correlation. Checked v6 save states include both complete memory cards and CPU, CD,
SPU, GPU, MDEC, scheduling, and memory state. Load validation precedes mutation;
a later host-file commit failure is reported and paused, with no filesystem
rollback promise.

`PCSX.encodeJSON` and `PCSX.jsonArray` own deterministic report encoding and array
tagging; `support` exposes direct aliases. GPU wrappers adapt their options into
one `PCSX.decodeGPU` or `PCSX.readGPUChain` call. Saved-state queries and comparisons
use `PCSX.selectSaveStateWire` to validate the complete input and decode only the
requested envelope with native `pb.decode`. Empty paths still decode the full
state; checked restore admission remains intact. Comparison shares cached schema
descriptors and uses native byte scanning throughout.

`History.records` and `History.cdWorkerRecords` copy frozen ranges into owning
zero-based arrays. `Audio.blocks` and `Audio.events` return batches of owning
named tables; `Audio.exportPCM` streams raw/WAV files in fixed chunks. Native
MMIO/VRAM packers construct complete store probes while Lua retains checked
admission, guest execution and scratch/register restoration. The scalar recorder
readers, Lua upload assembly and unused WAV helper are removed. See
[API](.agents/skills/psx-emulator/references/api.md) for bounds and
[native execution](.agents/skills/psx-emulator/references/native.md) for validation
rules. The mission catalogue is not fully performance-qualified.

Seven samples on one Linux x86-64 Release build, with host Lua JIT disabled,
compared the same inputs and file write/close operations:

| Caller operation | Previous Lua path | Native path |
| --- | ---: | ---: |
| Encode and export 10,000 JSON records | 29.13 ms | 7.08 ms (4.11×) |
| Decode and export 5,120 GPU words / 4,096 packets | 17.00 ms | 5.28 ms (3.22×) |
| Strict 20.4 MiB state decode and PC query | 5.17 ms | 5.00 ms |

The 21 report files, command stream and state input matched byte for byte.
State query timing stayed close to the baseline; native `pb.decode` remains in
both paths. These medians exclude startup, input acquisition, checked capture and
guest execution. They do not establish whole-mission speedups or predict other
hosts. Existing packet, ingress and strict-state checks also passed.

`PCSX.convertScreenshot(bytes, width, height, bpp)` converts packed BGR555 pixels
to RGB24 in one bounded call, sharing the native VRAM colour kernel. RGB24 passes
through unchanged. The frames mission keeps raw pixels and metadata and uses this
call for PPM export. Dimensions are limited to 1024×512 with exact input storage.

Seven alternating paired samples on one Linux x86-64 Release build, with host Lua
JIT disabled, measured PPM conversion, header construction, and file write/close.
Median times fell from 12.26 to 0.22 ms at 320×240 and from 93.55 to 1.70 ms at
1024×512, about 55× faster. Synthetic inputs covered all 65,536 BGR555 words;
output matched the old Lua loop. A live 640×478 BIOS display also matched. These
timings exclude guest execution and screenshot acquisition and do not establish
a whole-mission speedup or predict other hosts.

`PCSX.captureCPUChecked()` returns a copied CPU observation in one call. It retains
full checked-state admission and native validation, while avoiding Lua decoding of
the complete state. It includes GPR and CP0 banks, decimal cycle counts, delay and
exception context, and safe RAM/BIOS/scratch instruction words. It does not advance
the guest or read MMIO. Persisted-state inspection continues to use schema-aware
bounded decoding. See [API](.agents/skills/psx-emulator/references/api.md) and
[native execution](.agents/skills/psx-emulator/references/native.md).

A paused x86-64 interpreter comparison matched the previous Lua CPU capture at
13 RAM, BIOS, scratch, MMIO, alignment, and address boundaries, and retained the
running-state refusal. Seven paired samples of five calls gave medians of 201.6 ms
for Lua decoding and 198.2 ms for native projection. The gain was small; full
checked-state capture still dominates this operation. These results do not predict
other machines or guest workloads.

`PCSX.nextPause(function)` defers paused mission work until CPU execution and
event dispatch have unwound. Its native FIFO holds at most 64 callbacks, preserves
nested work, and rechecks the boundary before each callback. Target, step, GPU,
history, audio and CD capture use it while retaining checked-state refusals.

Receipts establish the supplied inputs, binary, limits, and captures for a run.
They do not establish hardware fidelity or deterministic replay. Describe the
observed behavior and its limits in the requesting project's evidence.

## Source layout

[Source](src/README.md) covers emulator owners, [tools](tools/README.md) covers
conversion utilities, and [CONTRIBUTING.md](CONTRIBUTING.md) covers local changes.
Vendor sources, PSX guest formats, hardware research, and upstream license notices
retain their original provenance.
