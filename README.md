# wperfk

[![ci](https://github.com/sumeetchhetri/wperfk/actions/workflows/ci.yml/badge.svg)](https://github.com/sumeetchhetri/wperfk/actions/workflows/ci.yml)

A modern HTTP benchmarking tool with Lua scripting that runs on Linux, macOS (Apple Silicon and Intel), Windows and FreeBSD. Prebuilt static binaries are published for every release.

wperfk brings together the best of the four tools most engineers reach for:

- **[wrk](https://github.com/wg/wrk)**: closed-loop, maximum-throughput load from a tiny C binary.
- **[wrk2](https://github.com/giltene/wrk2)**: constant-rate load with coordinated-omission-corrected latency.
- **[autocannon](https://github.com/mcollina/autocannon)**: pipelining, fixed request counts, warmup, bailout, rich result output.
- **[vegeta](https://github.com/tsenart/vegeta)**: target files, flexible rates, portable results that merge across machines.

Existing wrk/wrk2 Lua scripts run unmodified.

> **Status: pre-alpha.** P0 and P1 are done: wrk and wrk2 modes in one binary, the combined Lua API, autocannon/vegeta-style flags, and `--json` for [GATF](https://github.com/sumeetchhetri/gatf). P2 added native Windows builds (x64 and ARM64); P3 hardened the release pipeline. See the [roadmap](#roadmap).

## Why this exists

wrk and wrk2 are the best-engineered load generators around: small, multi-threaded C with no GC pauses skewing the numbers. But you can't use them on half the machines engineers actually have.

| Problem | wrk | wrk2 |
|---|---|---|
| Windows | ✗ POSIX-only | ✗ POSIX-only |
| Apple Silicon | Homebrew only | ✗ fails: LuaJIT 2.0.3 has no arm64 macOS support, `<x86intrin.h>`, x86-only `-pagezero_size`, Intel-only OpenSSL path |
| Prebuilt binaries | ✗ | ✗ |
| Constant-rate mode | ✗ | ✓ (mandatory) |
| Max-throughput mode | ✓ | ✗ |
| Maintained | last commit 2021 | last commit 2019 |

The runtime-based tools fill some gaps but bring their own ceilings:

- **autocannon** is single-threaded JavaScript per worker. Its own docs say it becomes CPU-bound and recommend wrk2 when that happens.
- **vegeta** is Go with a goroutine-per-request model and a GC. It is excellent for rate-based attacks, but has no scripting beyond static target lists.

So teams keep several tools and a Linux VM just to run one load test. wperfk aims to be **one binary, every mode, every OS**, keeping wrk's C engine and scripting while adopting the ergonomics that made autocannon and vegeta popular.

## Why not "wrk3"? (licence note)

In February 2015 wrk moved from Apache 2.0 to a **Modified Apache 2.0 License (v2.0.1)** (commit `db6da47`). The modification adds clause 4(e):

> If the Derivative Work includes substantial changes to features or functionality of the Work, then you must remove the name of the Work, and any derivation thereof, from all copies that you distribute…

A merged wrk + wrk2 is a substantial change, so any "wrk"-derived name would conflict with that clause if built on post-2015 wrk code. So:

- wperfk is **based on wrk2**, which is licensed under plain **Apache License 2.0**, and keeps its full git history.
- Features added to wrk after the licence change (`delay()`, callable `stats(p)`, etc.) are **re-implemented independently**, not copied.
- autocannon and vegeta (both MIT) inspire features only; no code is taken from either.
- The Lua global `wrk` (e.g. `wrk.method`, `wrk.format()`) is retained from wrk2's Apache 2.0 code for script compatibility.
- wperfk is licensed under **Apache License 2.0**. Upstream notices are preserved in [NOTICE](NOTICE).

This is our reading of the licences, not legal advice. If any upstream author sees it differently, please open an issue.

## Learnings adopted

### From autocannon

| Idea | wperfk feature | Phase |
|---|---|---|
| Native HTTP pipelining is its main throughput edge | `-p/--pipeline N` (no Lua script needed) | P1 |
| Stop after N requests, not N seconds | `-n/--requests N` | P1 |
| Warmup before sampling | `--warmup <T>` (warmup traffic excluded from stats) | P1 |
| Abort a broken run early | `--bailout N` errors | P1 |
| Per-status-code counts | `--status-codes` | P1 |
| Response validation | `--expect-status`, `--expect-body` → mismatch counter | P5 |
| Machine-readable results | `--json` | P5 |
| Live progress | `--progress` to stderr (stdout stays clean for JSON) | P5 |
| mTLS and SNI | `--cert`, `--key`, `--cacert`, `--sni`, `--verify` | P5 |
| Unix socket / Windows named pipe targets | `--unix-socket PATH` | P6 |
| Request sequences with state (login → token) | Lua `request()`/`response()`; example `scripts/login-token.lua` | P5 |
| Unique IDs in bodies | Example `scripts/unique-id.lua` (no new flag) | P5 |
| HAR import | `wperfk har2targets` converter | P6 |

### From vegeta

| Idea | wperfk feature | Phase |
|---|---|---|
| Flexible rate units (`50/1s`, `3000/1m`) | `-R 50/1s` in addition to plain req/s | P1 |
| Target files, no scripting required | `--targets FILE` (vegeta-compatible HTTP format), round-robin | P5 |
| Portable results that merge across machines | `--hist-out FILE` + `wperfk merge a b c` (exact HdrHistogram merge for distributed runs) | P5 |
| Live reporting interval | `--progress <T>` | P5 |
| DNS overrides | `--resolve host:port:addr` (curl-style) | P5 |
| Prometheus exporter | `--prometheus :9100` | P6 |

### Deliberately not adopted

- **Per-request result logs** (vegeta's gob/CSV stream): these cost memory and I/O at high rates. We keep HdrHistograms, which are fixed-size and exactly mergeable.
- **Programmatic JS/Go library APIs:** out of scope. Lua is our extension point.
- **HTTP/2 and HTTP/3:** not in v1. It is a large change to the connection engine and will be revisited after v1.

## Design

**Two load models, one tool**

| Invocation | Model |
|---|---|
| `wperfk -t4 -c100 -d30s URL` | Closed loop, max throughput (wrk semantics) |
| `wperfk -t4 -c100 -d30s -R 20000 URL` | Open loop, constant rate, coordinated-omission-corrected latency (wrk2 semantics) |

- HdrHistogram is used in both models.
- The output header names the active model, because closed-loop latency is **not** CO-corrected and must not be compared with `-R` results.
- All timing moves to a monotonic clock.

**Lua API**: the union of wrk and wrk2, with no changes to existing hooks:
`setup`, `init`, `request`, `response`, `delay`, `done`, `wrk.format`, `wrk.lookup`, `wrk.connect`, `wrk.time_us`, `thread:get/set/stop`, `stats(p)`.

**Portability**

| Layer | POSIX | Windows |
|---|---|---|
| Event loop | redis `ae`: epoll / kqueue / evport | `ae` + [wepoll](https://github.com/piscisaureus/wepoll) backend |
| Sockets | native | Winsock shim + fd-index table |
| Lua | LuaJIT 2.1 (pinned submodule) | same |
| TLS | OpenSSL 3.x | same, static |

## Install

From the first stable release:

```sh
# macOS and Linux (Homebrew)
brew tap sumeetchhetri/wperfk https://github.com/sumeetchhetri/wperfk
brew install wperfk

# Windows (Scoop)
scoop bucket add wperfk https://github.com/sumeetchhetri/wperfk
scoop install wperfk

# Windows (winget), once the package is accepted into winget-pkgs
winget install SumeetChhetri.wperfk
```

Or download an archive from [Releases](https://github.com/sumeetchhetri/wperfk/releases); every binary is a single static file.

Maintainers: each stable tag regenerates `Formula/`, `bucket/` and `winget/` with `tools/gen-packages.sh` and commits them to `main`. For winget, submit `winget/*.yaml` to [microsoft/winget-pkgs](https://github.com/microsoft/winget-pkgs) once by hand (under `manifests/s/SumeetChhetri/wperfk/<version>/`). After that, a `WINGET_TOKEN` repository secret (a classic PAT with `public_repo`) lets the release open the update PRs.

## Usage

```
wperfk -t4 -c100 -d30s --latency https://host/path            # closed loop (wrk)
wperfk -t4 -c100 -d30s -R 2000 --latency https://host/path    # constant rate (wrk2)
wperfk -t2 -c10 -d10s -m POST --body-file req.json \
       -H "Content-Type: application/json" --json https://host/api
```

| Option | Meaning |
|---|---|
| `-t -c -d -s -H -L/--latency -U -B --timeout` | Same as wrk / wrk2 |
| `-R, --rate <N>` | Constant rate: `2000`, `50/1s`, `3000/1m`, `5/100ms` (≥ 1 req/s). Omit for closed-loop max throughput |
| `-p, --pipeline <N>` | Pipeline N requests per connection (no Lua needed) |
| `-n, --requests <N>` | Stop after exactly N responses. Without `-d`, there is no time limit |
| `--warmup <T>` | Send traffic for T first, and exclude it from every result |
| `--bailout <N>` | Stop after N errors, with exit code 2 |
| `-m, --method <M>` | HTTP method |
| `--body <S>`, `--body-file <F>` | Request body (binary-safe from file) |
| `--json` | JSON summary on stdout. All other output, including Lua `print`, goes to stderr |
| `--progress <T>` | Live progress on stderr every T: requests, req/s, errors |
| `--targets <F>` | Send requests from a target file round-robin (see below). The URL argument becomes optional |
| `--status-codes` | Print the number of responses per status code |
| `--expect-status <L>` | Allowed status codes, e.g. `200,204` or `2xx,304`. Others count as mismatches |
| `--expect-body <S>` | Response body must contain S. Others count as mismatches |
| `--hist-out <F>` | Save the latency histograms and totals, for `wperfk merge` |
| `--verify` | Verify the server certificate and host name (off by default, as in wrk) |
| `--cacert <F>` | CA bundle for `--verify`; implies `--verify` |
| `--cert <F>`, `--key <F>` | Client certificate and key (PEM) for mutual TLS |
| `--sni <S>` | TLS server name for SNI and verification (default: URL host) |
| `--resolve <H:P:A>` | Connect to address A for host H port P, keeping H for Host, SNI and verification (curl syntax) |

CLI `-m/--body/-H` set defaults. A `-s` script can still override them.

**Exit codes:** 0 success, 1 usage or setup error, 2 `--bailout` reached, 3 `--expect-*` mismatches.

**Targets file** (vegeta's HTTP format). Targets are separated by a blank line, lines starting with `#` are comments, and `@file` sets the body. All targets must share one scheme, host and port. `-H` headers and `--body` apply to every target unless it sets its own:

```
GET https://api.example.com/users
Authorization: Bearer abc

POST https://api.example.com/users
Content-Type: application/json
@user.json
```

**Distributed runs:** run wperfk on several machines with `--hist-out node1.hist` etc., then combine them. Percentiles are merged exactly from the histogram buckets, not averaged:

```sh
wperfk merge node1.hist node2.hist node3.hist          # text report
wperfk merge --json node*.hist > combined.json
```

**TLS notes:** `--verify` without `--cacert` uses the system CA bundle on Linux, macOS and FreeBSD, and the Windows certificate store on Windows. With TLS 1.3, a server that rejects the client certificate does so after the handshake, so it shows up as read errors rather than connect errors.

**`--json` output** (one object, latencies in µs):

```json
{"tool":"wperfk","version":"0.1.0","url":"http://…","mode":"rate","rate":100,
 "threads":2,"connections":4,"duration_s":2,"runtime_us":2001234,
 "pipeline":1,"warmup_s":0,"requests_limit":0,"bailout":0,"stopped_by":"duration",
 "requests":202,"bytes":34436,"requests_per_sec":100.94,"bytes_per_sec":17207.8,
 "errors":{"total":0,"connect":0,"read":0,"write":0,"timeout":0,"status":0,"mismatch":0},
 "status_codes":{"200":202},"latency_unit":"us",
 "latency":{"min":…,"max":…,"mean":…,"stdev":…,"p50":…,"p75":…,"p90":…,"p99":…,"p99_9":…,"p99_99":…,"p99_999":…},
 "latency_uncorrected":{…}}
```

`stopped_by` is `duration`, `requests`, `bailout`, `signal`, or `merged` for `wperfk merge`. `mode` is `closed` or `rate`. In `closed` mode, `latency` and `latency_uncorrected` are identical. `errors.status` counts responses ≥ 400; `errors.mismatch` counts `--expect-*` failures.

## Release assets

Each `v*` tag publishes:

- `wperfk_<version>_<os>_<arch>.tar.gz`, or `.zip` for Windows. `os` is `linux`, `darwin` or `windows`; `arch` is `amd64` or `arm64`. The binary, LICENSE, NOTICE and README sit at the archive root.
- `wperfk_<version>_darwin_universal.tar.gz`: one macOS binary for both Intel and Apple Silicon.
- `wperfk_<version>_freebsd_amd64.tar.gz` (tier 2): published when the FreeBSD build passes; it never blocks a release.
- `wperfk_<version>_checksums.txt`, one `sha256  filename` line per archive (`sha256sum` format).
- GitHub build-provenance attestations for every archive.

| Platform | Linking | Runs on |
|---|---|---|
| Linux | fully static (musl) | any distro, glibc or musl |
| macOS | OpenSSL 3.5 LTS built from source and linked statically | macOS 11 Big Sur and later |
| Windows | fully static (MinGW, UCRT) | Windows 10 / Server 2016 and later; system DLLs only |
| FreeBSD | fully static | FreeBSD 14 and later |

Every binary is stripped, and each is smoke-tested before it is packaged. To dry-run the whole pipeline without publishing, run the `release` workflow manually from the Actions tab.

## Building from source

```sh
git clone --recursive https://github.com/sumeetchhetri/wperfk
cd wperfk
make                      # Linux: needs libssl-dev
make                      # macOS: needs `brew install openssl@3` (auto-detected)
make WITH_LUAJIT=/prefix  # optional: use an installed LuaJIT 2.1
make WITH_OPENSSL=/prefix # optional: use a specific OpenSSL
make STATIC=1 VERSION=x.y # release-style build (use Alpine/musl on Linux)
tests/smoke.sh ./wperfk   # HTTP, HTTPS and every script in scripts/
```

On Windows, build in an [MSYS2](https://www.msys2.org) UCRT64 shell (CLANGARM64 on ARM64):

```sh
pacman -S --needed make git zip mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-openssl
make STATIC=1                    # produces wperfk.exe; CLANGARM64: make STATIC=1 CC=clang
```

Cross-compiling for Windows from Linux (MinGW-w64) needs a host LuaJIT 2.1 for the embedded bytecode:

```sh
make STATIC=1 CC=x86_64-w64-mingw32-gcc-posix WITH_OPENSSL=/path/to/mingw/openssl \
     HOST_LUAJIT=/path/to/host/luajit \
     LUAJIT_MAKEFLAGS="CC=gcc HOST_CC=gcc CROSS=x86_64-w64-mingw32- TARGET_SYS=Windows"
```

## Roadmap

| Phase | Scope | Status |
|---|---|---|
| P0 | Bootstrap from wrk2; LuaJIT 2.1; Apple Silicon and OpenSSL 3 fixes; CI (Linux x64/arm64, macOS arm64) with smoke tests | ✅ done |
| P1a | Optional `-R` (closed loop without it), `-m`, `--body`, `--body-file`, `--json` with status-code breakdown, static-safe embedded Lua module, release workflow and asset naming | ✅ done |
| P1b | `delay()`, `stats(p)`, monotonic clock, `-p`, `-n`, `--warmup`, `--bailout`, rate units, 100 ms stop latency | ✅ done |
| P2 | Windows x64 and ARM64: wepoll backend, Winsock shims, Ctrl+C handling, CI and release | ✅ done |
| P3 | Release hardening: macOS 11+ builds (pinned OpenSSL from source), macOS universal binary, FreeBSD, stripped binaries, release dry run | ✅ done |
| P4 | Homebrew tap, Scoop bucket, winget | ✅ done |
| P5 | `--progress`, `--targets`, `--hist-out` + `merge`, `--expect-*`, `--status-codes`, mTLS/SNI/verify, `--resolve` | ✅ done |
| P6 | Unix sockets / named pipes, HAR converter, Prometheus exporter, llhttp to replace archived `http_parser` | next |

## Known limitations

- **Platform coverage follows LuaJIT:** x86/x64, arm64, ppc, mips. riscv64 is not supported upstream; a PUC Lua 5.1 fallback build is planned, but scripts using `ffi` won't run on it.
- **Windows throughput will be lower than Linux** on the same hardware. For high connection counts, widen the ephemeral port range: `netsh int ipv4 set dynamicport tcp start=10000 num=55535`. wepoll relies on the undocumented AFD interface, as libuv does.
- **On Windows, output uses CRLF line endings,** including `--json`. It is still valid JSON.
- **`http_parser` is archived upstream.** Replacing it with llhttp is planned for P6.

## Credits

wperfk exists because of **wrk** (Will Glozer), **wrk2** (Gil Tene, Mike Barker), **autocannon** (Matteo Collina and contributors) and **vegeta** (Tomás Senart and contributors). It also builds on:

- the redis `ae` event loop (Salvatore Sanfilippo)
- `http_parser` (Joyent / Node contributors)
- LuaJIT (Mike Pall)
- HdrHistogram C (Michael Barker)
- TinyMT (Saito & Matsumoto)
- wepoll (Bert Belder)

## License

[Apache License 2.0](LICENSE). Third-party notices are in [NOTICE](NOTICE).
