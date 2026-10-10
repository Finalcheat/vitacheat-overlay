#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
cmake -S tests -B build/host -DCMAKE_BUILD_TYPE=Debug "$@"
cmake --build build/host --parallel 4
ctest --test-dir build/host --output-on-failure
