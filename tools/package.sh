#!/usr/bin/env bash
# Usage: tools/package.sh <version> <os> <arch> [binary]
# Produces dist/wperfk_<version>_<os>_<arch>.tar.gz (.zip for windows)
# with the binary, LICENSE, NOTICE and README.md at the archive root.
set -euo pipefail
ver=$1 os=$2 arch=$3
bin=${4:-wperfk}; [ "$os" = windows ] && bin=${4:-wperfk.exe}
name="wperfk_${ver}_${os}_${arch}"
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
mkdir -p dist
cp "$bin" LICENSE NOTICE README.md "$stage/"
if [ "$os" = windows ]; then
  out="$PWD/dist/$name.zip"
  if command -v zip >/dev/null; then (cd "$stage" && zip -q -9 "$out" ./*)
  else (cd "$stage" && 7z a -tzip -bso0 "$out" ./*); fi   # Windows runners ship 7z, not zip
  echo "dist/$name.zip"
else
  tar -C "$stage" -czf "dist/$name.tar.gz" .
  echo "dist/$name.tar.gz"
fi
