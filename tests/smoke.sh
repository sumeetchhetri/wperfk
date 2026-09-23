#!/usr/bin/env bash
# Smoke test: plain HTTP, HTTPS, and every script in scripts/ must run
# without Lua errors and complete requests.
set -u
BIN=${1:-./wperfk}
PY=${PYTHON:-python3}
HTTP=18080; HTTPS=18443
tmp=$(mktemp -d)
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$tmp/k.pem" -out "$tmp/c.pem" \
  -days 1 -subj /CN=localhost >/dev/null 2>&1
$PY tests/server.py $HTTP </dev/null >/dev/null 2>&1 & p1=$!
$PY tests/server.py $HTTPS "$tmp/c.pem" "$tmp/k.pem" </dev/null >/dev/null 2>&1 & p2=$!
trap 'kill $p1 $p2 2>/dev/null; rm -rf "$tmp"' EXIT
sleep 2

fail=0
run() { # name, args...
  local name=$1; shift
  out=$("$BIN" -t2 -c4 -d2s -R100 "$@" 2>&1); rc=$?
  req=$(echo "$out" | sed -nE 's/^ *([0-9]+) requests in.*/\1/p' | head -1)
  if [ $rc -ne 0 ] || [ "${req:-0}" -eq 0 ] || echo "$out" | grep -qiE "stack traceback|attempt to"; then
    echo "FAIL $name rc=$rc requests=${req:-0}"; echo "$out"; fail=1
  else
    echo "ok   $name requests=$req"
  fi
}

run http  http://127.0.0.1:$HTTP/
run https https://localhost:$HTTPS/
for s in scripts/*.lua; do run "$s" -s "$s" http://127.0.0.1:$HTTP/; done
exit $fail
