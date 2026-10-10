# Memory inspection and patching

Run `--mission memory` ([source](../../../../src/lua/missions/memory.lua)) for
physical bytes without MMIO effects.

## Procedure

Select stopped context and `region=ram|scratch|rom` (RAM), zero-based `offset` (0),
`length` (256). Region sizes: 2 MiB, 1024 bytes, 512 KiB respectively.
Check `before.bin` and `memory.json`. Optional `hex=12345678` requires exactly
`length` byte pairs; RAM/scratch writes invalidate code caches and emit `after.bin`.
ROM writes refuse. `savestate=1` retains edits.

## Application

Verify bytes and receipt. Direct pointers do not model bus accesses, watchpoints,
caches or DMA. Device effects: [VRAM](vram.md), [SPU](spu.md), [CD-ROM](cdrom.md),
[Bus](bus.md). Side-effect-free device inspection: [States](states.md).
