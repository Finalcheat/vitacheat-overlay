#!/bin/sh
# Build a developer UI separately; never replace accepted release artifacts.
set -eu
cd "$(dirname "$0")/.."
: "${VITASDK:?Set VITASDK to your SDK path}"
export PATH="$VITASDK/bin:$PATH"
python3 scripts/generate_cp936.py --check
cmake -S . -B build/resource-profile -DCMAKE_BUILD_TYPE=Release -DVCO_RESOURCE_PROFILE=ON
cmake --build build/resource-profile --parallel 4 --target VitaCheatOverlayUI.suprx-self
python3 scripts/check_ui_palette.py --object build/resource-profile/CMakeFiles/shell_objects.dir/src/ui/overlay_view.cpp.obj
python3 - <<'PY'
from pathlib import Path
import hashlib
import json
import os
import re
import subprocess
root = Path.cwd()
directory = root / 'build/resource-profile'
nm = Path(os.environ['VITASDK']) / 'bin/arm-vita-eabi-nm'
symbols = subprocess.check_output([str(nm), str(directory / 'VitaCheatOverlayUI')], text=True)
for name in ('vco_resource_profile_tick', 'vco_resource_profile_visible'):
    if not re.search(r' T ' + name + r'$', symbols, re.MULTILINE):
        raise SystemExit('Missing developer sampling symbol: ' + name)
paths = [path for path in (root / 'src').rglob('*') if path.suffix in ('.c', '.cpp', '.h', '.inc', '.txt')]
paths += [root / 'CMakeLists.txt', root / 'scripts/build-resource-profile.sh', root / 'scripts/resource_profile.py']
hashes = lambda files: {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
                       for path in sorted(files)}
version = re.search(r'#define VCO_VERSION "([^"]+)"', (root / 'src/version.h').read_text()).group(1)
profile_header = (root / 'src/platform/resource_profile.h').read_text()
revision = int(re.search(r'#define VCO_RESOURCE_PROFILE_REVISION (\d+)', profile_header).group(1))
attempts = int(re.search(r'#define VCO_RESOURCE_PROFILE_START_ATTEMPTS (\d+)', profile_header).group(1))
marker = re.search(r'#define VCO_RESOURCE_PROFILE_START_MARKER "([^"]+)"', profile_header).group(1)
record = {'base_ui_version': version, 'resource_profile': True, 'developer_only': True,
          'profile_revision': revision, 'startup_attempt_limit': attempts, 'startup_marker': marker,
          'start_condition': 'first_ready_game_session',
          'interval_us': 5000000, 'window_us': 300000000, 'sample_limit': 60,
          'sources': hashes(paths),
          'artifacts': hashes([directory / 'VitaCheatOverlayUI.suprx', directory / 'vco-base.rco']),
          'native_destruction_barrier': False, 'device_tested': False}
(directory / 'profile-build.json').write_text(json.dumps(record, indent=2) + '\n')
print('Developer profile UI ready; normal release artifacts were not replaced.')
PY
