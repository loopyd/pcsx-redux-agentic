# Geometry coprocessor

Run `--mission gte` ([source](../../../../src/lua/missions/gte.lua)) for COP2
inspection, raw edits or native commands.

## Procedure

| Action | Operands / boundary |
| --- | --- |
| `inspect` (default) | Offline state or live boundary; 32 data and 32 control registers |
| `write` | Live paused context; `bank=data|control` (data), `register=0..31`, `value=U32` |
| `step` | Supported GTE opcode in RAM, outside delay slot, Status.CU2 enabled; native execution to PC+4 |

Establish original opcode/expected arithmetic independently. Disabled COP2,
non-GTE and reserved commands refuse. Writes/steps accept `savestate=1`.

## Application

Check `gte.json`: before/after banks, PC, opcode/command, signed MAC0–MAC3,
FLAG bits 12–31. Banks contain unsigned words, zero-based indices; use program
conventions for packed halfwords/fixed point.

Raw edits omit MTC2/CTC2 sign extension, FIFO and special-register effects;
execute guest instructions for those. Native stepping neither suppresses IRQs,
counts all retired instructions nor establishes hardware latency.
