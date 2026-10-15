# MDEC state and guest decode

Run `--mission mdec` ([source](../../../../src/lua/missions/mdec.lua)) to inspect
saved decoder state or capture a bounded guest command/DMA interval. Read
[Runtime](runtime.md) first. The action uses the existing native MDEC decoder;
it does not supply a second software decoder.

## Procedure

| Action | Inputs and observations |
| --- | --- |
| `inspect` (default) | Saved state, or live target/EXE. Reports command/status, compressed-input pointers, pending output DMA, quantization tables, scale matrix and partial-buffer position. |
| `export` | The same report plus all 768 serialized block-buffer bytes in `mdec-buffer.bin`. Unused buffer bytes are not decoded pixels. |
| `decode` | Caller-supplied guest EXE, or state with matching `--origin`. Requires `identity=LABEL` and aligned `stop=ADDRESS`. Runs the guest's command writes and DMA requests until that PC. |

Decode requires no pre-existing busy stream, pending output DMA or partial output
buffer. It does not reset the device or inject guest commands. Establish inputs in
the guest before its command/DMA stores. Table uploads and compressed data must
remain in standard 2 MiB RAM throughout the transfer. The report identifies the
observed command event and each input/output DMA request separately.

Bounds are `frames=1..36000` (1), `events=1..65536` (16384),
`bytes=0..8388608` (1048576), `commands=1..4096` (64) and
`words=1..2097152` (262144). Words count the sum of declared input and output DMA
words. Commands count observed writes to the command register. History's event
and payload budgets also include other devices. Frame exhaustion before `stop`,
unexpected pauses and any recorder failure prevent success.

Supported guest input commands are decode, 128-byte Y/UV quantization upload and
128-byte scale upload. Decode uses unsigned RGB24 or RGB555. RGB555 may set bit
15 through STP. Signed, monochrome and reserved flags refuse qualification. Coefficient runs
that exceed a block and incomplete six-block macroblocks also refuse. A
decode command's low 16-bit word count must match its input DMA product. Input
and output request modes use native CHCR `0x01000201` and `0x01000200`.

## Captures and acceptance

`initial.pbuf` and `final.pbuf` retain checked device/memory state. Pointer fields
use zero for null and otherwise backing offset plus one. Block-buffer position
uses the same encoding relative to its 768-byte array. Register values are
serialized observations, not live port reads.

`events.ndjson`, `payload.bin` and `events.json` preserve the frozen native
[History](history.md). `mdec-input.bin` concatenates actually consumed compressed
and table bytes in journal order. Unconsumed input and peeks are not appended.
`mdec-output.bin` concatenates actual output spans in journal order. Transfer
entries in `mdec.json` join both files' offsets to native sequence, request,
command and input-owner identities. A buffered copy retains its original decode
input identity. DMA order does not imply image-wide macroblock tiling.

Output blocks are 16 by 16 pixels. Each block is row-major RGB24, or little-endian
RGB555 with red in bits 0–4, green in 5–9, blue in 10–14 and STP in bit 15.
RGB24 has 768 bytes per block; RGB555 has 512. A successful selected stream needs
observed DMA completion, exact output/table counts, captured decode owners and
no remaining busy stream or partial output buffer. Completion is checked
separately from produced bytes. Truncated decode input can complete native DMA
without producing its requested output; that result fails qualification.

Failed captures retain their available raw journal and decoded byte prefix.
`decode_error` or `capture_error` identifies failed qualification; a failed report
never publishes success. Compare exact output bytes with an independently
calculated fixture and retain its source identity. Transport success alone does
not establish pixel correctness. The existing scale-matrix model, color rounding,
black-and-white setting and DMA timing are not hardware fidelity guarantees.
Quantization, scale and partial-output observations support continuation analysis;
this action does not claim a streaming video container decoder.
