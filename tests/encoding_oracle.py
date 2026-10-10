#!/usr/bin/env python3
"""Test every CP936 pair against the original mapping, independent of its C table."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
subprocess.run([sys.executable, str(ROOT / 'scripts/generate_cp936.py'), '--check'], check=True)
mapping = {}
for line in (ROOT / 'vendor/unicode/CP936.TXT').read_text('ascii').splitlines():
    fields = line.split('#', 1)[0].split()
    if len(fields) == 2:
        source, value = map(lambda text: int(text, 16), fields)
        mapping[source] = chr(value).encode('utf-8')
records = bytearray()
for lead in range(0x81, 0xff):
    for trail in range(0x40, 0xff):
        if trail == 0x7f:
            continue
        expected = mapping.get(lead * 256 + trail, b'')
        # CPython's independent GBK codec agrees for every assigned pair.
        if expected:
            assert bytes([lead, trail]).decode('gbk').encode('utf-8') == expected
        records.extend(bytes([lead, trail, len(expected)]) + expected.ljust(3, b'\0'))
with tempfile.TemporaryDirectory() as directory:
    fixture = Path(directory) / 'pairs.bin'
    fixture.write_bytes(records)
    subprocess.run([sys.argv[1], 'oracle', str(fixture)], check=True)
