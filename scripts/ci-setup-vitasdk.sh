#!/bin/sh
# Pinned Linux SDK from the official VitaSDK autobuilds release.
set -eu
: "${VITASDK:?Set the new SDK destination}"
test "$(uname -s)" = Linux
test "$(uname -m)" = x86_64
if [ -e "$VITASDK" ]; then
  echo 'SDK destination already exists; refusing to overwrite it.' >&2
  exit 1
fi
sdk_url=https://github.com/vitasdk/autobuilds/releases/download/master-linux-v2.533/vitasdk-x86_64-linux-gnu-2025-06-09_13-39-09.tar.bz2
sdk_sha=3f2d818f9b85f24e8db0434c552481be67a0af9dcaf05fd22e5f9425199088b0
sdk_temporary=$(mktemp -d)
trap 'rm -rf "$sdk_temporary"' EXIT HUP INT TERM
curl --fail --silent --show-error --location --retry 3 "$sdk_url" --output "$sdk_temporary/sdk.tar.bz2"
printf '%s  %s\n' "$sdk_sha" "$sdk_temporary/sdk.tar.bz2" | sha256sum --check -
mkdir "$sdk_temporary/unpacked"
tar -xjf "$sdk_temporary/sdk.tar.bz2" -C "$sdk_temporary/unpacked"
test -x "$sdk_temporary/unpacked/vitasdk/bin/arm-vita-eabi-gcc"
test -f "$sdk_temporary/unpacked/vitasdk/share/vita.cmake"
mkdir -p "$(dirname "$VITASDK")"
mv "$sdk_temporary/unpacked/vitasdk" "$VITASDK"
"$VITASDK/bin/arm-vita-eabi-gcc" --version
