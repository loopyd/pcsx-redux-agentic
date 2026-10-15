# Memory cards

Run `--mission memcard` ([source](../../../../src/lua/missions/memcard.lua)).
Slots: `1`, `2`; `slot` defaults to `1`.
The default action is `directory`; `icons=1` includes decoded icon frames.
`read` exports zero-based `offset` (0), `length` (remainder) within the 131072-byte
image to `card.bin`; `file` exports the save starting
at `block=1..15` to `save.bin`. Reports use `psx.runtime-memcard/v1`.

`copy` and `move` require `block` and a different `destination` slot. `erase`
requires `block`. `import` reads explicit `--input card=PATH` with
`format=auto|raw|vgs|dexdrive` (auto). `export` writes a raw `card.mcd`. `flush` writes
the slot's configured backing file. Edits and imports mark the live image dirty;
they do not flush it. Guest card traffic can later trigger the existing automatic
flush. `savestate=1` captures the resulting full machine state.

## Guest capture and continuation

`action=capture` starts at the EXE entry or `target`. It requires a word-aligned
`stop` PC and counts at most `frames=1..36000` Vsyncs, default 180. The stop is a
pre-instruction observation at a settled paused boundary. A frame deadline retains
the final boundary and fails. Use fresh harness output and the portable backing
files `memcard1.mcd` and `memcard2.mcd`; never configure an original input as a
working backing file.

An optional `--input card=PATH` imports into `slot` before the initial capture.
The harness keeps that staged input separate from backing files and checks its
hash. Both boundaries export both complete raw card images, checked v6 states,
CPU registers and card/SIO status in `memcard.json`, schema
`psx.runtime-memcard-capture/v1`. Optional `address` and `length` export a guest RAM
transcript at both boundaries. The range must fit physical 2 MiB RAM through
segments 0, 4 or 5; length defaults to 256. The guest must record the transmitted,
received and status bytes needed to interpret its protocol.

`block=1..15` additionally extracts `save.bin` at the successful stop. `flush=1`
attempts explicit persistence afterward and reports the resulting dirty status.
Extraction or flush failure retains boundary captures and its error, and fails
without a completion marker. Neither option is required for an interrupted serial
capture; active card transactions refuse edits, import and explicit flush while
checked capture remains available.

Continue an interrupted transaction with the successful capture's `--origin` and
`--input state=.../final.pbuf`. Supply no EXE, target or card reimport. Native
checked restore resumes the saved PC, both images, FIFO and protocol progress.
Restoration marks cards dirty. The next guest card byte can automatically flush
that image before the pending transaction completes; a successful guest sector
write can also automatically flush it. Compare exported card bytes and guest
responses separately from backing-file persistence.

For acceptance, use an isolated allocated save and a guest that writes a known
sector, rereads it and retains the full serial transcript in RAM. Compare the raw
image and extracted save to independent expected bytes, and verify the original
input hash. Stop halfway through a write, continue from its checked state, and
compare the completed protocol and image with an uninterrupted run. Include a
bad-checksum command, truncated import, unsafe edit at the interrupted boundary,
and failed flush to an unwritable working destination. Preserve the refusal,
images and dirty status. A successful host extraction alone does not prove guest
save success or hardware fidelity.

## Native image operations

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
`memorycardlua.{h,cc}`. Native save-state v6 includes both images and card
protocol progress. Host edits and extraction establish bytes and filesystem
structure, not guest save success, PocketStation execution or hardware fidelity.
