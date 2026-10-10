#!/usr/bin/env python3
"""Record/check the exact local sources and artifacts from a successful build."""
import hashlib
import json
import re
from pathlib import Path
import sys
ROOT = Path(__file__).resolve().parents[1]
PLUGINS = ('VitaCheatOverlayKernel.skprx', 'VitaCheatOverlayUI.suprx', 'VitaCheatOverlayShell.suprx',
           'VitaCheatOverlayGame.suprx')


def hashes(paths):
    return {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(paths)}


def current():
    cache = (ROOT / 'build/CMakeCache.txt').read_text()
    if not re.search(r'^VCO_RESOURCE_PROFILE:BOOL=OFF$', cache, re.MULTILINE):
        raise SystemExit('Developer resource profile must not be packaged as a normal release; run scripts/build.sh')
    sources = [path for path in (ROOT / 'src').rglob('*') if path.suffix in ('.c', '.cpp', '.h', '.txt', '.inc')]
    sources += [ROOT / name for name in ('CMakeLists.txt', 'kernel-exports.yml', 'game-exports.yml', 'ui-exports.yml',
                                        'shell-exports.yml', 'scripts/paf_stubs.py', 'vendor/taihen/imports.yml',
                                        'vendor/taihen/taihen.h', 'scripts/build.sh', 'scripts/build_stamp.py',
                                        'scripts/generate_cp936.py', 'vendor/unicode/CP936.TXT',
                                        'scripts/generate_ui_resources.py', 'scripts/rco.py',
                                        'scripts/ui_assets.py', 'scripts/check_ui_palette.py', 'scripts/check_game_self.py', 'scripts/check_syscall_abi.py',
                                        'resources/base.rco', 'resources/overlay.json')]
    return {'configuration': {'resource_profile': False}, 'sources': hashes(sources),
            'artifacts': hashes([ROOT / 'build' / name for name in (*PLUGINS, 'vco-base.rco')])}


def harness():
    sources = [path for path in (ROOT / 'tools/device-harness').rglob('*') if path.is_file()]
    sources += [ROOT / 'scripts/build-harness.sh', ROOT / 'scripts/build_stamp.py']
    cache = (ROOT / 'build/harness/CMakeCache.txt').read_text()
    match = re.search(r'^VCO_SDK_COMMON:PATH=(.+)$', cache, re.MULTILINE)
    if not match:
        raise SystemExit('Missing harness SDK source configuration')
    common = Path(match.group(1))
    upstream = {name: hashlib.sha256((common / name).read_bytes()).hexdigest()
                for name in ('debugScreen.c', 'debugScreen.h', 'debugScreenFont.c', 'debugScreen_custom.h')}
    return {'sources': hashes(sources), 'sdk_debug_screen': upstream,
            'artifacts': hashes([ROOT / 'build/harness/VcoDeviceHarness.vpk'])}


def main():
    action = sys.argv[1] if len(sys.argv) == 2 else ''
    if action not in ('record', 'check', 'record-harness', 'check-harness'):
        raise SystemExit('Usage: build_stamp.py record|check|record-harness|check-harness')
    is_harness = action.endswith('-harness')
    stamp = ROOT / ('build/harness-build.json' if is_harness else 'build/plugin-build.json')
    value = harness() if is_harness else current()
    if action.startswith('record'):
        stamp.write_text(json.dumps(value, sort_keys=True, indent=2) + '\n')
    else:
        if not stamp.exists() or json.loads(stamp.read_text()) != value:
            script = 'build-harness.sh' if is_harness else 'build.sh'
            raise SystemExit('Sources or artifacts changed; run scripts/' + script)


if __name__ == '__main__':
    main()
