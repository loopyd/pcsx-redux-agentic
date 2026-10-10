# Saved-state inspection

Run `--mission state` ([source](../../../../src/lua/missions/state.lua)) to
query/export/compare without loading. Use running-build schema and bundled
`pb`/`protoc`; no guessed offsets or external parser.

## Procedure

Choose action, schema path and bounds; inspect a parent/schema for unknown names.
Validate receipt, schema and input hashes before reusing bytes.

| Action | Inputs / arguments | Captures |
| --- | --- | --- |
| `schema` | No state | `state.proto` |
| `query` (default) | `--input state=PATH`; optional `path`, `depth` | `query.json` |
| `export` | State, bytes-field `path`; optional `offset`, `length` | `region.bin`, `region.json` |
| `compare` | State and `--input other=PATH`; optional subtree `path`, `depth`, `limit`; native byte comparison | `comparison.json`, `state.proto` |

Use fork-owned v6 raw checked states from `savestate=1`; GUI gzip rejects, even renamed.
Equal versions do not establish build compatibility.

Paths use dotted names and **one-based** indices: `registers.pc`,
`registers.gpr[1]`, `spu.channel[1].adsr_ex`, `memory.ram`. Quote brackets in shell
arguments. Empty path selects root; unknown names/invalid indices fail.
Queries/exports reject absence; comparisons report it.

Query `depth=0..8` (1); zero lists message field/type/presence or array counts.
Beyond 4096 expanded nodes, narrow the query. Previews disclose truncation at
32 bytes; large integers retain `#decimal` strings.

Export `offset` is zero-based; `length` defaults to the remainder and must fit.
`memory.ram` is 8 MiB: use `length=2097152` to compare with `ram.bin`.
`spu.ram` is 512 KiB. Serialized regions imply neither bus aliases nor meaningful
guest memory throughout.

<a id="structured-comparison"></a>

Comparison (`psx.runtime-comparison/v2`) accepts any field or whole state, reporting
paths, one-based indices, zero-based contiguous byte ranges and ≤32 preview
bytes/side. Preview truncation preserves counts/ranges. Absence differs from
encoded zero/false; both presence flags survive when both fields are absent.

Comparison `depth=0..64` (16), `limit=1..65536` (1024); traversal caps at 100000
nodes and 64 MiB compared byte positions. Exceeded bounds fail without partial
equality/completion; narrow the subtree. `offset`/`length` are export-only.

Before comparison, `PCSX.validateSaveStateWire` rejects unknown fields, incompatible wire types,
duplicate singular fields, truncation, overflowing varints/
narrow integers and non-0/1 booleans. Tags plus scalar elements (including packed)
cap at 100000. Native wire validation is bounded to 64 MiB and 64 nesting levels;
Selection retains requested field wire occurrences; bundled
`pb.decode` supplies presence, defaults and exact uint64 values without decoding
unrelated state buffers. Empty paths still decode the whole state. Query mode
retains unknown-field skipping and last singular occurrence behavior; malformed
known fields elsewhere still reject. Defaults apply only inside present parents.
Version validation uses a separate envelope. Wire inspection neither loads state
nor replaces checked-restore admission.

## Application

Retain `state.proto` and origin receipts. Structural compatibility proves neither
build identity, complete hardware serialization nor repeatability.

<a id="restore-and-branch-an-experiment"></a>

Execution actions validate full native checked state and restore via
`snapshot`. Boundary: restored PC without target/EXE, explicit target after
resumption, or EXE header entry by default. Omit EXE for immediate inspection.

Restoration replaces ROM/program memory; bootstrap BIOS hash does not identify
embedded ROM. Inputs stay unchanged; `savestate=1` writes a new `state.pbuf`.
Equality proves neither cycle accuracy, hidden-device restoration nor host
scheduling/audio replay.

Midstream XA restore requires saved decoder readiness, frequency, bit depth,
channel mode and sample count. Checked load rejects states missing that format
when a pending sector needs it. Capture a new state with the current build.
