#!/usr/bin/env python3
"""Check the actual Game SELF, including VCO's file-backed RW segment policy.

ELF permits BSS-only segments. VCO deliberately avoids that layout in its Vita
plugin: passing this check is an artifact contract, not proof of device loading.
"""
from pathlib import Path
import json
import struct
import sys
import zlib

ROOT = Path(__file__).resolve().parents[1]


def check(data):
    def unpack(fmt, offset):
        size = struct.calcsize(fmt)
        if offset < 0 or offset + size > len(data):
            raise ValueError('Truncated SELF metadata')
        return struct.unpack_from(fmt, data, offset)

    if len(data) < 0x80 or data[:4] != b'SCE\0':
        raise ValueError('Not a Vita SELF')
    elf, phdr = unpack('<QQ', 0x40)
    segments, = unpack('<Q', 0x58)
    if data[elf:elf + 7] != b'\x7fELF\x01\x01\x01':
        raise ValueError('Expected ELF32 little-endian payload')
    elf_type, machine = unpack('<HH', elf + 16)
    entry_size, count = unpack('<HH', elf + 42)
    if elf_type != 0xfe04 or machine != 40 or entry_size != 32 or count != 3:
        raise ValueError('Unexpected Game SELF layout')
    records = []
    for index in range(count):
        kind, offset, address, physical, filesz, memsz, flags, alignment = unpack(
            '<8I', phdr + index * entry_size)
        start, size, compression, encryption = unpack('<4Q', segments + index * 32)
        if start + size > len(data) or encryption != 2 or filesz > 0x1000000:
            raise ValueError('Invalid SELF segment bounds/encoding')
        payload = data[start:start + size]
        if compression == 2:
            inflater = zlib.decompressobj()
            decoded = inflater.decompress(payload, filesz + 1)
            if (not inflater.eof or inflater.unconsumed_tail or
                    len(inflater.unused_data) > 3 or any(inflater.unused_data)):
                raise ValueError('Invalid compressed segment')
        elif compression == 1:
            decoded = payload[:filesz]
            if len(payload) - filesz > 3 or any(payload[filesz:]):
                raise ValueError('Invalid plain segment padding')
        else:
            raise ValueError('Unknown compression')
        if len(decoded) != filesz:
            raise ValueError('ELF/SELF segment size mismatch')
        if kind == 1:
            if not memsz or filesz > memsz:
                raise ValueError('Invalid PT_LOAD segment size')
            if flags & 2 and not filesz:
                raise ValueError('Game RW segment is BSS-only; initialize worker.thread to -1')
            records.append({'index': index, 'flags': flags, 'filesz': filesz, 'memsz': memsz})
    if len(records) != 2 or records[0]['flags'] != 5 or records[1]['flags'] != 6:
        raise ValueError('Expected Game RX and RW segments')
    return {'load_segments': records, 'file_backed_rw': True}


if __name__ == '__main__':
    try:
        path = Path(sys.argv[1]) if len(sys.argv) == 2 else ROOT / 'build/VitaCheatOverlayGame.suprx'
        print(json.dumps(check(path.read_bytes())))
    except (OSError, ValueError, struct.error, zlib.error) as error:
        raise SystemExit('Game SELF check failed: ' + str(error))
