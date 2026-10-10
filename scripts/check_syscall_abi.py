#!/usr/bin/env python3
"""Reject exported syscalls that require C stack arguments on 32-bit ARM."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def check(root=ROOT):
    exports = re.findall(r'^\s+- (\w+): 0x[0-9A-Fa-f]+$',
                         (root/'kernel-exports.yml').read_text(), re.M)
    headers = '\n'.join(p.read_text() for p in sorted((root/'src').rglob('*.h')))
    headers = re.sub(r'/\*.*?\*/|//[^\n]*', '', headers, flags=re.S)
    records = []
    for name in exports:
        found = re.findall(r'\bint\s+'+name+r'\s*\(([^()]*)\)\s*;', headers)
        if len(found) != 1:
            raise ValueError(f'{name}: expected one raw syscall declaration')
        words = 0
        for argument in found[0].split(','):
            argument = argument.strip()
            if not argument or argument == 'void':
                continue
            if '*' in argument:
                words += 1
            elif re.search(r'\b(?:uint64_t|int64_t)\b', argument):
                words = (words+1) & ~1  # AAPCS even-register alignment.
                words += 2
            elif re.match(r'(?:u?int(?:8|16|32)_t|int|unsigned(?: int)?|bool)\b', argument):
                words += 1
            else:
                raise ValueError(f'{name}: unknown argument type: {argument}')
        records.append((name, words))
    bad = [f'{name} ({words} words)' for name, words in records if words > 4]
    if bad:
        raise ValueError('Syscall arguments exceed r0-r3: '+', '.join(bad))
    if not records:
        raise ValueError('No syscall exports found')
    return records


if __name__ == '__main__':
    try:
        records = check()
    except ValueError as exc:
        raise SystemExit(str(exc))
    print(f'ARM syscall ABI check passed: {len(records)} exports fit r0-r3.')
