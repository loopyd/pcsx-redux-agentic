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
These are separate from the managed emulator build below.

## Run agentic missions

In a BOF3 checkout, the `pcsx-redux` component builds this submodule with the
approved SDL3 prefix and records the binary, source revisions, embedded resources,
and native state schema:

```sh
bin/harness setup --component pcsx-redux
bin/harness runtime status
bin/harness runtime missions
bin/harness runtime run --bios inputs/external/bios/scph5501.bin --mission smoke --output out/runtime/smoke --timeout 30
```

Create the output parent first and use a fresh output directory. Supply your own
licensed BIOS, executables, and disc images. Keep proprietary inputs and captures
local. The binary embeds 19 entry missions and 35 shared modules; no loose canonical
Lua files are required at runtime. `--script` remains available for trusted external
Lua. Lua runs as host code and is not a sandbox.

The [psx-emulator skill](skills/psx-emulator/SKILL.md) owns mission selection,
arguments, runtime bounds, capture interpretation, and native API procedures.
Install or check its links from an integrating project with:

```sh
third_party/pcsx-redux-agentic/scripts/manage-psx-emulator-skill.sh install .
third_party/pcsx-redux-agentic/scripts/manage-psx-emulator-skill.sh check .
```

The manager links the fork-owned skill at the project's `.pi` location and
maintains existing `.agents` and `.codex` aliases. It does not install dependencies
or modify home hooks.

## Native APIs

Use native bulk operations for state capture, memory-card readback, byte-range
comparison, and VRAM conversion. Keep Lua responsible for mission composition and
reports. Checked v6 save states include both complete memory cards and CPU, CD,
SPU, GPU, MDEC, scheduling, and memory state. Load validation precedes mutation;
a later host-file commit failure is reported and paused, with no filesystem
rollback promise.

`PCSX.captureCPUChecked()` returns a copied CPU observation in one call. It retains
full checked-state admission and native validation, while avoiding Lua decoding of
the complete state. It includes GPR and CP0 banks, decimal cycle counts, delay and
exception context, and safe RAM/BIOS/scratch instruction words. It does not advance
the guest or read MMIO. Persisted-state inspection continues to use schema-aware
bounded decoding. See [API](skills/psx-emulator/references/api.md) and
[native execution](skills/psx-emulator/references/native.md).

A paused x86-64 interpreter comparison matched the previous Lua CPU capture at
13 RAM, BIOS, scratch, MMIO, alignment, and address boundaries, and retained the
running-state refusal. Seven paired samples of five calls gave medians of 201.6 ms
for Lua decoding and 198.2 ms for native projection. The gain was small; full
checked-state capture still dominates this operation. These results do not predict
other machines or guest workloads.

`PCSX.nextPause(function)` defers paused mission work until CPU execution and
event dispatch have unwound. Its native FIFO holds at most 64 callbacks, preserves
nested work, and rechecks the boundary before each callback. Target and step
captures use this scheduler while retaining checked-state refusals.

Receipts establish the supplied inputs, binary, limits, and captures for a run.
They do not establish hardware fidelity or deterministic replay. Describe the
observed behavior and its limits in the requesting project's evidence.

## Source layout

[Source](src/README.md) covers emulator owners, [tools](tools/README.md) covers
conversion utilities, and [CONTRIBUTING.md](CONTRIBUTING.md) covers local changes.
Vendor sources, PSX guest formats, hardware research, and upstream license notices
retain their original provenance.
