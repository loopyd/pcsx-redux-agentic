# PCSX-Redux Agentic

PCSX-Redux Agentic is a Linux fork of [PCSX-Redux](https://github.com/grumpycoders/pcsx-redux)
for agentic coding, PlayStation reverse engineering, and behavioral analysis. It
combines the emulator and debugger with bounded native capture APIs and an embedded
Lua mission library. The interface and built-in messages use English (`en_US`) only.
The settings loader ignores saved `Locale` keys and omits them when saving.
Japanese font support remains for PlayStation memory-card titles.

The upstream authorship and licenses remain in [AUTHORS](AUTHORS),
[LICENSE](LICENSE), and [LICENSES.md](LICENSES.md).

## Build locally

Clone the fork with its pinned submodules:

```sh
git clone --recursive https://github.com/loopyd/pcsx-redux-agentic.git
make -C pcsx-redux-agentic -j6 BUILD=Release
```

The supported host build is native Linux Make with a C++23 compiler.
Required pkg-config packages are `capstone`, `freetype2`, `libavcodec`, `libavformat`,
`libavutil`, `libswresample`, `libcurl`, `libuv`, `sdl3`, and `zlib`. The GUI also
links OpenGL, X11, and XCB. Other libraries come from the pinned submodules or
compatible system packages selected by the Makefile. Install dependencies through
your project's approved toolchain process. The fork does not publish cloud builds
or maintain hosted CI. The Linux host graph has x86-64 and AArch64 code paths.

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

The binary embeds the missions and their shared Lua modules. Query their names,
roles, compiled Lua, and native state schema without starting emulation:

```sh
./pcsx-redux -mission-catalog
```

Select a builtin with `-mission NAME`, or trusted external Lua with `-dofile FILE`.
Both routes preload the embedded shared modules. The canonical Lua files are
embedded in the binary. Lua runs as host code without a sandbox.

Host integrations supply per-run settings, staged inputs and arguments, process
bounds, cleanup, and receipt validation. Supply your own licensed BIOS,
executables, and disc images, and keep proprietary inputs and captures local.
The [psx-emulator skill](.agents/skills/psx-emulator/SKILL.md) documents mission
selection, captures, and native APIs. Harness build and invocation instructions
belong in the integrating repository.

## Native APIs

Lua composes missions; native bulk APIs handle capture, decoding, comparison and
export. The [API reference](.agents/skills/psx-emulator/references/api.md) owns
signatures and bounds. The [native guide](.agents/skills/psx-emulator/references/native.md)
owns build and validation rules.

Checked v6 save states include both complete memory cards and CPU, CD, SPU, GPU,
MDEC, scheduling, and memory state. The loader validates the input before mutation.
If a later host-file commit fails, the loader reports the failure and pauses the
emulator. It does not roll back filesystem changes.

| Capability | Reference |
| --- | --- |
| Schema-aware state queries, export and comparison | [States](.agents/skills/psx-emulator/references/states.md) |
| GPU packets, ordering tables, VRAM and display | [GPU](.agents/skills/psx-emulator/references/gpu.md), [VRAM](.agents/skills/psx-emulator/references/vram.md), [Frames](.agents/skills/psx-emulator/references/frames.md) |
| Frozen device journals and streaming PCM/WAV export | [History](.agents/skills/psx-emulator/references/history.md), [Audio](.agents/skills/psx-emulator/references/audio.md) |
| Checked CD observations, response waits, media controls and ISO listing | [CD-ROM](.agents/skills/psx-emulator/references/cdrom.md) |
| Owned card snapshots, inventory, extraction, edits, import/export and flush | [Memory cards](.agents/skills/psx-emulator/references/memcard.md) |
| JSON, probe packing, CPU observations, screenshots and deferred pause work | [API](.agents/skills/psx-emulator/references/api.md) |

Receipts establish the supplied inputs, binary, limits, and captures for a run.
They do not establish hardware fidelity or deterministic replay. Describe the
observed behavior and its limits in the requesting project's evidence.

## Source layout

[Source](src/README.md) covers emulator owners, [tools](tools/README.md) covers
conversion utilities, and [CONTRIBUTING.md](CONTRIBUTING.md) covers local changes.
Vendor sources, PSX guest formats, hardware research, and upstream license notices
retain their original provenance.
