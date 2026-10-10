#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
: "${VITASDK:?Set VITASDK to your SDK path}"
export PATH="$VITASDK/bin:$PATH"
python3 scripts/generate_cp936.py --check
python3 scripts/generate_ui_resources.py --build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DVCO_RESOURCE_PROFILE=OFF
cmake --build build --parallel 4
python3 scripts/check_ui_palette.py
python3 scripts/check_game_self.py
python3 scripts/check_syscall_abi.py
python3 scripts/build_stamp.py record
