#!/usr/bin/env bash
# Smoke test: plain HTTP, HTTPS, and every script in scripts/ must run
# without Lua errors and complete requests.
set -u
BIN=${1:-./wperfk}
PY=${PYTHON:-python3}
HTTP=18080; HTTPS=18443
tmp=$(mktemp -d)
# MSYS2 would rewrite /CN=... as a path for native openssl.exe; no-op elsewhere.
MSYS2_ARG_CONV_EXCL='/CN=' openssl req -x509 -newkey rsa:2048 -nodes -keyout "$tmp/k.pem" -out "$tmp/c.pem" \
  -days 1 -subj /CN=localhost >/dev/null 2>&1
$PY tests/server.py $HTTP </dev/null >/dev/null 2>&1 & p1=$!
MSYS2_ARG_CONV_EXCL='/CN=' openssl req -x509 -newkey rsa:2048 -nodes -keyout "$tmp/ck.pem" -out "$tmp/cc.pem" \
  -days 1 -subj /CN=wperfk-client >/dev/null 2>&1
$PY tests/server.py $HTTPS "$tmp/c.pem" "$tmp/k.pem" </dev/null >/dev/null 2>&1 & p2=$!
MTLS=18444   # requires a client certificate
$PY tests/server.py $MTLS "$tmp/c.pem" "$tmp/k.pem" "$tmp/cc.pem" </dev/null >/dev/null 2>&1 & p3=$!
trap 'kill $p1 $p2 $p3 2>/dev/null; rm -rf "$tmp"' EXIT
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

# P5: targets, expectations, histograms + merge, progress, TLS options, --resolve
native() { command -v cygpath >/dev/null && cygpath -m "$1" || echo "$1"; }  # MSYS2: file contents aren't path-converted
hits() { $PY -c "import json,urllib.request as u;print(json.dumps(json.load(u.urlopen('http://127.0.0.1:$HTTP/__hits'))))"; }
printf 'hello-wperfk' > "$tmp/tbody"
cat > "$tmp/targets.txt" <<EOF
# two targets, sent round-robin
GET http://127.0.0.1:$HTTP/t-get

POST http://127.0.0.1:$HTTP/expect-post
X-Test: 1
@$(native "$tmp/tbody")
EOF
$PY -c "import urllib.request as u;u.urlopen('http://127.0.0.1:$HTTP/__reset')"
expect targets          "d['requests']>0 and set(d['status_codes'])=={'200'}" -t1 -c2 -d1s --targets "$tmp/targets.txt"
if hits | $PY -c "import json,sys; h=json.load(sys.stdin); a,b=h.get('GET /t-get',0),h.get('POST /expect-post',0); sys.exit(0 if a>0 and b>0 and abs(a-b)<=4 else 1)"
then echo "ok   targets-round-robin"; else echo "FAIL targets-round-robin"; hits; fail=1; fi
expect expect-status    "rc==3 and d['errors']['mismatch']==d['requests']>0"    -t1 -c2 -d1s --expect-status 204 $B
expect expect-body      "rc==0 and d['errors']['mismatch']==0 and d['requests']>0" -t1 -c2 -d1s --expect-body token --expect-status 2xx $B
expect hist-a           "d['requests']>0" -t1 -c2 -d1s --hist-out "$tmp/a.hist" $B
cp "$tmp/out.json" "$tmp/a.json"
expect hist-b           "d['requests']>0" -t1 -c2 -d1s --hist-out "$tmp/b.hist" $B
"$BIN" merge --json "$tmp/a.hist" "$tmp/b.hist" > "$tmp/m.json" 2>"$tmp/err"
if $PY - "$tmp/a.json" "$tmp/out.json" "$tmp/m.json" <<'PYEOF2'
import json, sys
a, b, m = (json.load(open(f)) for f in sys.argv[1:])
assert m['requests'] == a['requests'] + b['requests'] and m['stopped_by'] == 'merged'
assert m['latency']['max'] == max(a['latency']['max'], b['latency']['max'])
PYEOF2
then echo "ok   merge"; else echo "FAIL merge"; cat "$tmp/m.json" "$tmp/err"; fail=1; fi
if "$BIN" -t1 -c2 -d2s --progress 1s $B 2>&1 | grep -q "\[run"; then echo "ok   progress"; else echo "FAIL progress"; fail=1; fi
expect tls-verify-fail  "d['requests']==0 and d['errors']['connect']>0" -t1 -c2 -d1s --verify https://localhost:$HTTPS/
expect tls-cacert       "d['requests']>0 and d['errors']['total']==0"   -t1 -c2 -d1s --cacert "$tmp/c.pem" https://localhost:$HTTPS/
expect tls-sni          "d['requests']>0 and d['errors']['total']==0"   -t1 -c2 -d1s --cacert "$tmp/c.pem" --sni localhost https://127.0.0.1:$HTTPS/
expect mtls-no-cert     "d['requests']==0"                              -t1 -c2 -d1s https://localhost:$MTLS/
expect mtls-cert        "d['requests']>0 and d['errors']['total']==0"   -t1 -c2 -d1s --cert "$tmp/cc.pem" --key "$tmp/ck.pem" https://localhost:$MTLS/
expect resolve          "d['requests']>0 and d['errors']['total']==0"   -t1 -c2 -d1s --resolve wperfk.invalid:$HTTP:127.0.0.1 http://wperfk.invalid:$HTTP/

# P6: response framings (llhttp), dropped connections, Unix sockets, HAR, Prometheus
for f in chunked close http10 lf 204 big; do
  expect "framing-$f" "d['requests']>0 and d['errors']['total']==0" -t1 -c2 -d1s http://127.0.0.1:$HTTP/$f
done
# server drops keep-alive connections: counted as read errors, must not spin
expect framing-drop     "d['requests']>50 and d['errors']['read']>0"    -t1 -c2 -d1s http://127.0.0.1:$HTTP/drop

# Windows AF_UNIX is not yet verified, so it is opt-in there (WPK_TEST_UNIX=1) and
# never blocks a release. WPK_SKIP_UNIX=1 skips it anywhere (e.g. Wine lacks AF_UNIX).
if [ -z "${WPK_SKIP_UNIX:-}" ] && { [ "${OS:-}" != Windows_NT ] || [ -n "${WPK_TEST_UNIX:-}" ]; } &&
   $PY -c "import socket,sys; sys.exit(0 if hasattr(socket,'AF_UNIX') else 1)"; then
  $PY tests/server.py "unix:$tmp/s.sock" </dev/null >/dev/null 2>&1 & p4=$!
  sleep 1
  expect unix-socket    "d['requests']>0 and d['errors']['total']==0"   -t1 -c2 -d1s --unix-socket "$tmp/s.sock" http://localhost/
  kill $p4 2>/dev/null
else
  echo "skip unix-socket (no AF_UNIX test server here)"
fi

$PY - "$tmp/t.har" $HTTP <<'PYEOF2'
import json, sys
u = "http://127.0.0.1:%s" % sys.argv[2]
e = lambda m, url, h=(), body=None: {"request": {"method": m, "url": url,
     "headers": [{"name": k, "value": v} for k, v in h],
     **({"postData": {"mimeType": "text/plain", "text": body}} if body else {})}}
json.dump({"log": {"version": "1.2", "entries": [
    e("GET", u + "/har-get", [(":authority", "x"), ("Accept", "*/*")]),
    e("POST", u + "/expect-post", [("X-Test", "1"), ("Content-Length", "999")], "hello-wperfk"),
    e("GET", "https://cdn.example.com/skipped.js")]}}, open(sys.argv[1], "w"))
PYEOF2
$PY -c "import urllib.request as u;u.urlopen('http://127.0.0.1:$HTTP/__reset')"
expect har              "d['requests']>0 and set(d['status_codes'])=={'200'}" -t1 -c2 -d1s --targets "$tmp/t.har"
if hits | $PY -c "import json,sys; h=json.load(sys.stdin); a,b=h.get('GET /har-get',0),h.get('POST /expect-post',0); sys.exit(0 if a>0 and b>0 and abs(a-b)<=4 else 1)"
then echo "ok   har-round-robin"; else echo "FAIL har-round-robin"; hits; fail=1; fi

PROM=18990
"$BIN" -t1 -c2 -d3s --prometheus 127.0.0.1:$PROM $B >/dev/null 2>&1 & pp=$!
sleep 1.5
if $PY - $PROM <<'PYEOF2'
import sys, urllib.request as u
body = u.urlopen("http://127.0.0.1:%s/metrics" % sys.argv[1], timeout=5).read().decode()
vals = dict(l.rsplit(" ", 1) for l in body.splitlines() if l and not l.startswith("#"))
assert float(vals["wperfk_requests_total"]) > 0 and vals["wperfk_running"] == "1", body
assert "# TYPE wperfk_latency_seconds summary" in body
PYEOF2
then echo "ok   prometheus"; else echo "FAIL prometheus"; fail=1; fi
wait $pp
exit $fail
