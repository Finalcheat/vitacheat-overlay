#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
: "${VITASDK:?Set VITASDK to your SDK path}"
export PATH="$VITASDK/bin:$PATH"
cmake -S tools/device-harness -B build/harness -DCMAKE_BUILD_TYPE=Release
cmake --build build/harness --parallel 4
python3 scripts/build_stamp.py record-harness
