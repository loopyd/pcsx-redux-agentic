# Memory cards

Read [runtime](runtime.md), then use `--mission memcard`. Slots are `1` and `2`.
The default action is `directory`; `icons=1` includes decoded icon frames.
`read` exports a bounded range to `card.bin`, and `file` exports the save starting
at `block=1..15` to `save.bin`. Reports use `psx.runtime-memcard/v1`.

`copy` and `move` require `block` and a different `destination` slot. `erase`
requires `block`. `import` reads an explicit `--input card=PATH`; `format` accepts
`auto`, `raw`, `vgs` or `dexdrive`. `export` writes a raw `card.mcd`. `flush` writes
the slot's configured backing file. Edits and imports mark the live image dirty;
they do not flush it. Guest card traffic can later trigger the existing automatic
flush. `savestate=1` captures the resulting full machine state.

Live operations require full checked-state admission at a settled paused owner
boundary. Running execution, active recorders, pending loads, busy devices and
leases refuse. Edits, import and flush also refuse an active card serial
transaction. Copy and move prepare both affected images before a nonthrowing
in-memory commit. Host writes are separate; no transaction across two card files
or crash-durable persistence is promised. A failed flush leaves the card dirty.

For trusted Lua, `PCSX.MemoryCards.captureChecked(slot, offset?, length?)` returns
`image`, `bytes`, range metadata and compact card/SIO `status`. The image owns a
complete 131072-byte snapshot; `image:bytes()` returns its raw bytes. Offset and
length must fit that image. Use one image for repeated queries:

```lua
local cards = PCSX.MemoryCards
local capture = cards.captureChecked(1, 0, 0)
local inventory = cards.directory(capture.image, {icons = false})
local bytes, metadata = cards.readFile(capture.image, 1)
```

`directory`, `readFile` and `export` also accept a live slot or exact raw image
bytes. Snapshot and byte queries do not touch the emulator. Inventory reports
malformed entries in `diagnostics`; extraction rejects invalid checksums,
allocation states, links, cycles, shared chains and declared lengths. Chains have
at most 15 distinct 8192-byte blocks; extracted data is at most 122880 bytes.
Icons are opt-in little-endian BGR555 frames; title decoding remains native.

`editChecked({operation, source, block, destination?})`,
`importChecked(slot, bytes, format?)` and `flushChecked(slot)` return metadata or
raise checked-operation errors. Import normalizes a complete supported container
and validates its directory before mutation; it never creates a missing input.
`export(source, path, {overwrite=false, format='raw'})` checks open, write and close
results. Existing destinations refuse unless overwrite is explicit. Export failure
can leave a partial host file; it does not change live card bytes.

The native owners are `src/core/memorycard.{h,cc}`, `sio.{h,cc}` and
`memorycardlua.{h,cc}`. Native save-state v6 already includes both images and card
protocol progress. Host edits and extraction establish bytes and filesystem
structure, not guest save success, PocketStation execution or hardware fidelity.
