#!/usr/bin/env bash
# Usage: tools/verify-release.sh <dir> <version> <os_arch>   (e.g. linux_amd64, darwin_universal)
# Checks one released asset as a user would get it: checksum, archive layout,
# version, platform properties (static / system-only dylibs / macOS 11 / fat),
# and that it generates load in wrk mode, wrk2 mode and with a Lua script.
# Run from a checkout (uses tests/server.py and scripts/post.lua).
set -euo pipefail
dir=$1 ver=$2 plat=$3
src=$PWD
PY=$(command -v python3 || command -v python)
fail() { echo "::error::$plat: $*"; exit 1; }
ok()   { echo "ok   $plat: $*"; }

cd "$dir"
sums="wperfk_${ver}_checksums.txt"
[ -f "$sums" ] || fail "missing $sums"
grep -E "  wperfk_${ver}_${plat}\.(tar\.gz|zip)$" "$sums" > one.sum || fail "not listed in $sums"
asset=$(awk '{print $2}' one.sum)
[ -f "$asset" ] || fail "missing $asset"
if command -v sha256sum >/dev/null; then sha256sum -c one.sum; else shasum -a 256 -c one.sum; fi \
  || fail "checksum mismatch"
ok "checksum $asset"

rm -rf x && mkdir x
case $asset in
  *.zip) if [ "${OS:-}" = Windows_NT ]; then
           pwsh -NoProfile -Command "Expand-Archive -Path '$asset' -DestinationPath x"
         else unzip -q "$asset" -d x; fi ;;
  *)     tar xzf "$asset" -C x ;;
esac
exe=; case $plat in windows_*) exe=.exe ;; esac
bin=x/wperfk$exe
for f in LICENSE NOTICE README.md "wperfk$exe"; do [ -f "x/$f" ] || fail "archive lacks $f"; done
ok "archive layout"

v=$("$bin" -v | head -1)
case "$v" in "wperfk $ver "*) ok "version: $v" ;; *) fail "-v says '$v', want wperfk $ver" ;; esac

case $plat in
  linux_*|freebsd_*)
    file "$bin" | grep -q 'statically linked' || fail "not static: $(file "$bin")" ;;
  darwin_*)
    libs=$(otool -L "$bin" | grep -E '^\s' | grep -vE '^\s+/(usr/lib|System/Library)/' || true)
    [ -z "$libs" ] || fail "non-system dylibs: $libs"
    minos=$(otool -arch all -l "$bin" | awk '/minos/{print $2}' | sort -u | tr '\n' ' ')
    [ "$minos" = "11.0 " ] || fail "minos '$minos', want 11.0"
    if [ "$plat" = darwin_universal ]; then
      archs=$(lipo -archs "$bin")
      [ "$archs" = "x86_64 arm64" ] || fail "lipo archs '$archs'"
    fi ;;
esac
ok "platform checks"

port=$((20000 + RANDOM % 20000))
"$PY" "$src/tests/server.py" "$port" & srv=$!
trap 'kill $srv 2>/dev/null || true' EXIT
"$PY" - "$port" <<'PY' || fail "test server did not start"
import socket, sys, time
for _ in range(100):
    try: socket.create_connection(('127.0.0.1', int(sys.argv[1])), 0.2).close(); sys.exit(0)
    except OSError: time.sleep(0.1)
sys.exit(1)
PY
url=http://127.0.0.1:$port/
load() {   # name, wperfk args...
  local name=$1; shift
  "$bin" -t1 -c2 -d2s --json "$@" > "$name.json" 2> "$name.log" || { cat "$name.log"; fail "$name: wperfk exited $?"; }
  "$PY" - "$name.json" "$ver" <<'PY' || fail "$name: bad result $(cat "$name.json")"
import json, sys
d = json.load(open(sys.argv[1]))
assert d["version"] == sys.argv[2], d["version"]
assert d["requests"] > 0 and d["errors"]["total"] == 0, d
assert set(d["status_codes"]) == {"200"}, d["status_codes"]
PY
  ok "$name: $("$PY" -c "import json,sys;d=json.load(open(sys.argv[1]));print(d['requests'],'requests')" "$name.json")"
}
load wrk-mode  "$url"
load wrk2-mode -R 200 "$url"
load lua       -s "$src/scripts/post.lua" "$url"
