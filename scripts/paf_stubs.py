#!/usr/bin/env python3
"""Generate only the PAF imports referenced by our compiled objects."""
import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys

emd, output, *objects = sys.argv[1:]
sdk = Path(os.environ['VITASDK']) / 'bin'
output = Path(output)
output.mkdir(parents=True, exist_ok=True)
symbols = set()
for obj in objects:
    text = subprocess.check_output([str(sdk/'arm-vita-eabi-nm'), '-u', obj], text=True)
    symbols.update(line.split()[-1] for line in text.splitlines() if line.split())
libraries = {}
for path in sorted(Path(emd).glob('*.emd')):
    text = path.read_text(encoding='utf-8-sig')
    lib = re.search(r'Library: (\w+) libnamenid: (0x[0-9a-fA-F]+)', text)
    if not lib:
        continue
    functions, variables = {}, {}
    for kind, name in re.findall(r'Library: \w+ (function|variable): (\S+)', text):
        if name in symbols:
            nid = int.from_bytes(hashlib.sha1((name+'PAF').encode()).digest()[:4], 'little')
            (functions if kind == 'function' else variables)[name] = nid
    if functions or variables:
        libraries[lib[1]] = (lib[2], functions, variables)
lines = ['modules:', '  VcoPaf:', '    libraries:']
for name, (nid, functions, variables) in libraries.items():
    lines += [f'      {name}:', '        kernel: false', '        version: 1', f'        nid: {nid}']
    for kind, entries in [('functions', functions), ('variables', variables)]:
        if entries:
            lines += [f'        {kind}:']
            lines += [f'          {symbol}: 0x{value:08X}' for symbol, value in entries.items()]
database = output/'imports.yml'
database.write_text('\n'.join(lines)+'\n')
checks = ['/* Generated from the same PAF imports as the UI payload. */',
          'static const struct { unsigned library, function; } paf_checks[] = {']
for _, (nid, functions, _) in libraries.items():
    checks += [f'    {{ {nid}, 0x{value:08X} }},' for value in functions.values()]
checks += ['};']
(output/'paf_checks.h').write_text('\n'.join(checks)+'\n')
subprocess.run([str(sdk/'vita-libs-gen'), str(database), str(output)], check=True)
# ar updates existing archives without removing obsolete members. Old imports
# would otherwise reorder the UI's stubs versus a clean build even though its
# object code and current import set are identical.
(output/'libVcoPaf_stub.a').unlink(missing_ok=True)
subprocess.run(['make', '-s', '-C', str(output), 'libVcoPaf_stub.a'], check=True)
print('PAF imports:', sum(len(f)+len(v) for _, f, v in libraries.values()))
