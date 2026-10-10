#!/usr/bin/env python3
"""Reject UI colors that require a C++ startup pass absent from this suprx."""
import argparse
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--object', type=Path, default=ROOT / 'build/CMakeFiles/shell_objects.dir/src/ui/overlay_view.cpp.obj')
    args = parser.parse_args()
    nm = Path(os.environ['VITASDK']) / 'bin/arm-vita-eabi-nm'
    symbols = subprocess.check_output([str(nm), '-S', '-C', str(args.object)], text=True)
    uninitialized = []
    for line in symbols.splitlines():
        fields = line.split(maxsplit=3)
        if len(fields) == 4 and fields[2] in ('b', 'B') and fields[3].startswith('palette::'):
            uninitialized.append(fields[3])
    if uninitialized:
        raise SystemExit('UI palette requires constructors that module_start does not run: ' + ', '.join(uninitialized))
    print('ARM UI palette check passed: no color object requires startup initialization.')


if __name__ == '__main__':
    main()
