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

# P1b: -n, rate units, delay(), -p, --warmup, --bailout, stats(p)
expect() { # name, python-expr over d (JSON) and rc, args...
  local name=$1 cond=$2; shift 2
  "$BIN" --json "$@" 2>"$tmp/err" > "$tmp/out.json"; local rc=$?
  if $PY -c "import json,sys; d=json.load(open(sys.argv[1])); rc=int(sys.argv[2]); sys.exit(0 if ($cond) else 1)" "$tmp/out.json" $rc
  then echo "ok   $name"; else echo "FAIL $name (rc=$rc)"; cat "$tmp/out.json" "$tmp/err"; fail=1; fi
}
B=http://127.0.0.1:$HTTP/
printf 'function delay() return 100 end\n' > "$tmp/delay.lua"
cat > "$tmp/stats.lua" <<'LUA'
done = function(s, latency, r)
  assert(latency(99) == latency:percentile(99))
  io.stderr:write("STATS_OK\n")
end
LUA
expect requests-exact   "d['requests']==50 and d['stopped_by']=='requests' and rc==0" -t2 -c4 -n 50 $B
expect rate-units       "d['rate']==50 and 80<=d['requests']<=120"                   -t1 -c2 -d2s -R 3000/1m $B
expect delay            "10<=d['requests']<=25"                                        -t1 -c1 -d2s -s "$tmp/delay.lua" $B
expect pipeline         "d['pipeline']==4 and set(d['status_codes'])=={'200'}"        -t1 -c2 -d1s -p 4 $B
expect warmup           "80<=d['requests']<=120 and d['runtime_us']<2500000"          -t1 -c2 -R 50 --warmup 1s -d 2s $B
expect bailout          "d['stopped_by']=='bailout' and d['errors']['total']==5 and rc==2" -t2 -c4 -d30s --bailout 5 http://127.0.0.1:$HTTP/expect-post
expect stats-call       "open(sys.argv[1]).read() and True"                            -t1 -c2 -d1s -s "$tmp/stats.lua" $B
grep -q STATS_OK "$tmp/err" || { echo "FAIL stats-call (assert)"; cat "$tmp/err"; fail=1; }
exit $fail
