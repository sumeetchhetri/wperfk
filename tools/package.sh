#!/usr/bin/env bash
# Usage: tools/package.sh <version> <os> <arch> [binary]
# Produces dist/wperfk_<version>_<os>_<arch>.tar.gz (.zip for windows)
# with the binary, LICENSE, NOTICE and README.md at the archive root.
# The packaged binary is stripped (STRIP overrides the tool, e.g. for cross
# builds; STRIP=: disables it) and, when it can run here, smoke-run with -v.
set -euo pipefail
ver=$1 os=$2 arch=$3
bin=${4:-wperfk}; [ "$os" = windows ] && bin=${4:-wperfk.exe}
name="wperfk_${ver}_${os}_${arch}"
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
mkdir -p dist
cp "$bin" LICENSE NOTICE README.md "$stage/"
exe="$stage/$(basename "$bin")"

strip_tool=${STRIP:-$(command -v strip || command -v llvm-strip || true)}
if [ -n "$strip_tool" ]; then
  "$strip_tool" "$exe"
else
  echo "warning: no strip tool found; packaging an unstripped binary" >&2
fi
if [ "$(uname -s)" = Darwin ]; then
  codesign --force --sign - "$exe"   # arm64 needs a valid (ad-hoc) signature after strip
fi
if [ -z "${CROSS_PACKAGE:-}" ]; then
  "$exe" -v >/dev/null   # the stripped binary must still start
fi
if [ "$os" = windows ]; then
  out="$PWD/dist/$name.zip"
  if command -v zip >/dev/null; then (cd "$stage" && zip -q -9 "$out" ./*)
  else (cd "$stage" && 7z a -tzip -bso0 "$out" ./*); fi   # Windows runners ship 7z, not zip
  echo "dist/$name.zip"
else
  # Name the files: archiving "." adds a ./ entry with mktemp's 0700 mode,
  # which tar applies to the directory the user extracts into.
  tar -C "$stage" -czf "dist/$name.tar.gz" "$(basename "$bin")" LICENSE NOTICE README.md
  echo "dist/$name.tar.gz"
fi
