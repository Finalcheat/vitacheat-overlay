#!/bin/sh
# Fetch a pinned external dependency; no Vita or account access.
set -eu
cd "$(dirname "$0")/.."
commit=7b1d3b6aca81131fd90c65b81f317909e4feda1c
destination=.cache/paf-re
if [ -e "$destination" ]; then
  if [ ! -d "$destination/.git" ]; then
    echo 'PAF cache has no Git provenance; move it aside and rerun setup.' >&2
    exit 1
  fi
else
  mkdir -p .cache
  git clone https://github.com/GrapheneCt/ScePaf-RE.git "$destination"
  git -C "$destination" checkout --detach "$commit"
fi
test "$(git -C "$destination" rev-parse HEAD)" = "$commit"
test -z "$(git -C "$destination" status --porcelain)"
echo 'Pinned PAF dependency is ready.'
