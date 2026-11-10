#!/usr/bin/env python3
"""Write the original 80-byte little-endian History records from an HST1 journal."""

import argparse
import gzip
import struct
import sys
from contextlib import ExitStack

HEADER = struct.Struct("<4sI")
PACKED = struct.Struct("<QQQ11I")
RECORD = struct.Struct("<QQQQ12I")
TARGET = struct.Struct("<6Q")
BATCH = 4096
MASK = (1 << 64) - 1


def unpack(source, destination, payload=None, payload_destination=None):
    magic, count = HEADER.unpack(source.read(HEADER.size))
    if magic != b"HST1" or count > 67108864:
        raise ValueError("invalid HST1 header")
    payload_size = 2147483648
    if payload:
        magic, payload_size = HEADER.unpack(payload.read(HEADER.size))
        if magic != b"HSP1" or payload_size > 2147483648:
            raise ValueError("invalid HSP1 header")

    cycle = delta = offset = 0
    identities = {}
    targets = {}
    for first in range(0, count, BATCH):
        rows = min(BATCH, count - first)
        columns = source.read(rows * PACKED.size)
        if len(columns) != rows * PACKED.size:
            raise ValueError("short HST1 block")
        output = bytearray(rows * RECORD.size)
        payload_output = bytearray()
        for index in range(rows):
            step, request, related, kind, device, *data, length = PACKED.unpack(
                columns[index::rows]
            )
            if not 1 <= kind <= 50 or offset + length > payload_size:
                raise ValueError("invalid HST1 kind or payload range")
            key = (kind, min(device, 8))
            previous_request, previous_related = identities.get(key, (0, 0))
            request = (previous_request + request) & MASK
            related = (previous_related + related) & MASK
            identities[key] = request, related
            delta = (delta + step) & MASK
            cycle = (cycle + delta) & MASK
            RECORD.pack_into(
                output, index * RECORD.size, first + index + 1, cycle, request,
                related, kind, device, *data, offset, length,
            )
            if payload and length:
                raw = payload.read(length)
                if len(raw) != length:
                    raise ValueError("short HSP1 payload")
                if kind == 37 and device == 4:
                    slot = data[0]
                    prior = targets.get(slot, (0,) * 6)
                    values = tuple((a + b) & MASK for a, b in zip(prior, TARGET.unpack(raw)))
                    targets[slot] = values
                    raw = TARGET.pack(*values)
                payload_output.extend(raw)
            offset += length
        destination.write(output)
        if payload_destination:
            payload_destination.write(payload_output)
    if source.read(1):
        raise ValueError("trailing bytes in HST1 journal")
    if payload and (offset != payload_size or payload.read(1)):
        raise ValueError("HSP1 payload length differs from HST1 records")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("journal", help="events.hst.gz from a CD capture")
    parser.add_argument("--payload", help="payload.hst.gz from the same capture")
    parser.add_argument("--payload-output", help="new path for original payload bytes")
    args = parser.parse_args()
    if bool(args.payload) != bool(args.payload_output):
        parser.error("--payload and --payload-output must be used together")
    try:
        with ExitStack() as files:
            source = files.enter_context(gzip.open(args.journal, "rb"))
            payload = files.enter_context(gzip.open(args.payload, "rb")) if args.payload else None
            destination = files.enter_context(open(args.payload_output, "xb")) if payload else None
            unpack(source, sys.stdout.buffer, payload, destination)
    except (OSError, EOFError, ValueError, struct.error) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
