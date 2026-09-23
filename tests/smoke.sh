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

# closed loop (no -R), wrk-compatible flags
out=$("$BIN" -t2 -c4 -d2s --latency http://127.0.0.1:$HTTP/ 2>&1)
if echo "$out" | grep -q "closed loop" && ! echo "$out" | grep -q nan; then
  echo "ok   closed-loop"; else echo "FAIL closed-loop"; echo "$out"; fail=1; fi

# --json + request options without Lua: stdout must be pure JSON, all 200s
printf 'hello-wperfk' > "$tmp/body"
check_json() { # name, expected status code, args...
  local name=$1 code=$2; shift 2
  "$BIN" -t1 -c2 -d1s --json "$@" 2>/dev/null > "$tmp/out.json"
  if $PY - "$tmp/out.json" "$code" <<'PYEOF'
import json, sys
d = json.load(open(sys.argv[1]))
code = sys.argv[2]
assert d["requests"] > 0, d
assert set(d["status_codes"]) == {code}, d["status_codes"]
for k in ("requests_per_sec", "bytes", "errors", "latency", "latency_uncorrected", "mode"):
    assert k in d, k
PYEOF
  then echo "ok   $name"; else echo "FAIL $name"; cat "$tmp/out.json"; fail=1; fi
}
U=http://127.0.0.1:$HTTP/expect-post
check_json json-cli-request 200 -m POST --body-file "$tmp/body" -H "X-Test: 1" $U
check_json json-cli-negative 400 --body-file "$tmp/body" -H "X-Test: 1" $U
check_json json-rate-mode 200 -R50 http://127.0.0.1:$HTTP/
check_json json-lua-print 200 -s scripts/report.lua http://127.0.0.1:$HTTP/
exit $fail
