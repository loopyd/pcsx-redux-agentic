---
name: psx-emulator
description: Run bounded Linux PCSX-Redux Agentic missions for PS1 debugging, state queries, device capture, replay, CD-ROM and memory-card operations. Use for runtime evidence and native Lua API work.
---

# PSX emulator

Ships with Linux PCSX-Redux Agentic. Run embedded missions through
`bin/harness runtime run --mission NAME`. Harness owns bounds, cleanup and
receipts; requesting project owns semantic acceptance.
Every mission reads [runtime contract](references/runtime.md), then selected
reference below. Load only needed branches; [catalog](references/catalog.md)
covers discovery and context selection.

## Select an action

| Task | Read before work |
| --- | --- |
| Startup verification | [Startup](references/startup.md) |
| Stop, capture or restore | [Target](references/target.md) |
| CPU registers | [CPU](references/cpu.md) |
| Physical memory | [Memory](references/memory.md) |
| Breakpoints/access observations | [Watch](references/watch.md) |
| Stepping/exception context | [Step](references/step.md) |
| Execution coverage | [Trace](references/trace.md) |
| Saved-state queries/export/comparison | [States](references/states.md) |
| DMA configuration | [DMA](references/dma.md) |
| Interrupt latches/masks | [IRQ](references/irq.md) |
| Timer state | [Timers](references/timers.md) |
| Native DMA/IRQ/timer capture | [History](references/history.md) |
| Bus observations/MMIO probes | [Bus](references/bus.md) |
| Function invocation/selected-PC trace | [Call](references/call.md) |
| Frame advance/controller input/display | [Frames](references/frames.md) |
| Repeated continuation/scheduled input | [Replay](references/replay.md) |
| VRAM/textures/palettes | [VRAM](references/vram.md) |
| GPU packets/ordering tables/capture | [GPU](references/gpu.md) |
| Geometry coprocessor | [GTE](references/gte.md) |
| Sound voices/DSP state | [SPU](references/spu.md) |
| Native mixed PCM/SPU access capture | [Audio](references/audio.md) |
| Disc media/controller/journal capture | [CD-ROM](references/cdrom.md) |
| Card inventory, save extraction, edits and persistence | [Memory cards](references/memcard.md) |
| Extend Lua action | [API contract](references/api.md) |
| Native customization | [Native authorization and validation](references/native.md) |

## Execute

1. Check prerequisites; preserve dirty work and failed evidence. Prefer existing
   missions and state queries over new runners/parsers.
2. Define inputs, observations and a stop condition. Follow Runtime for
   staging, fresh output, preloaded helpers and trusted external Lua.
3. Verify receipts, then interpret captures. Distinguish guest execution, host
   edits and serialized state; success proves neither behavior nor fidelity.
4. Return input/tool/capture identities, commands, outcome, stop condition,
   coverage, uncertainties, method and wall time. Parent archives measurements.

Keep proprietary inputs/captures local. Static analysis -> `psx-rizin`; source
promotion and delegated scope -> requesting project. Harness never launches models.
Capability changes require updated procedures and affected-mission validation.
References hold reusable procedures/contracts; progress and experiment results
belong in project evidence. User docs and observation ledgers are not runtime inputs.
