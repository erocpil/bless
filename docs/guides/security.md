# Security

Bless is written in C and sends untrusted traffic patterns onto the wire -- memory safety
is a first-class concern, not an afterthought.  This document describes the sanitizer
coverage, fuzzing strategy, and CI enforcement in place.

---

## Address Sanitizer (ASan) + Undefined Behaviour Sanitizer (UBSan)

**Status**: clean, enforced in CI.

Every push to `main` runs the unit test suite under `clang -fsanitize=address,undefined`.
The ASan/UBSan build catches:

- Heap buffer overflows / over-reads
- Use-after-free
- Double-free
- Signed integer overflow
- Null pointer dereference
- Misaligned access
- Shift-out-of-bounds

The sanitizer variant runs as a separate CI step **before** the release build (see
`.github/workflows/ci.yml` lines 57-68).  If either `test_dist_asan` or `test_core_asan`
exits non-zero, the build fails.

### Running locally

```bash
clang -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  -I include -o test/unit/test_dist_asan test/unit/test_dist.c src/dist.c -lm
./test/unit/test_dist_asan

clang -fsanitize=address,undefined -fno-omit-frame-pointer -g \
  -I include -o test/unit/test_core_asan test/unit/test_core.c -lm
./test/unit/test_core_asan
```

To build the full bless binary with sanitizers for a smoke test:

```bash
make BUILD=asan
./build/asan/bin/bless conf/config-ci.yaml -- --num=100
```

---

## Fuzzing

**Status**: libFuzzer harness for core parsers; not yet integrated into OSS-Fuzz.

`fuzz/fuzz_bless.c` provides a libFuzzer entry-point (`LLVMFuzzerTestOneInput`) that
exercises four standalone functions with random byte input:

| Target | Description |
|--------|-------------|
| `make_power_of_2()` | Integer rounding -- boundary values (0, 1, UINT32_MAX) |
| `icmp_calc_cksum()` | One's-complement checksum -- arbitrary buffer |
| `fuzz_parse_port_range()` | Port range parser -- `PORT[+RANGE]` syntax |
| `fuzz_parse_ip_range()` | IP range parser -- `A.B.C.D[+RANGE]` syntax |

The parsers are local stand-alone copies (no DPDK dependency), so the fuzzer runs
on any Linux host.

### Running locally

```bash
make fuzz
```

This builds the harness and runs 60 seconds of fuzzing.  Any crash or sanitizer
failure exits non-zero.

### What the fuzzer covers (and what it does not)

**Covered:**
- IP/port range parser robustness (the most likely input-attack surface)
- Checksum computation on arbitrary-length buffers
- Integer boundary handling in power-of-2 rounding

**Not yet covered:**
- YAML config parser (`src/config.c`) -- libyaml's own parser handles YAML syntax,
  but Bless's semantic config parsing could benefit from structure-aware fuzzing
- Packet construction (`bless_mbufs_*`, `proto_quic.c`, `proto_sctp.c`) -- requires
  DPDK mbuf pools, so a stand-alone fuzzer is more involved
- VXLAN encapsulation / decapsulation
- WebSocket message handling
- Flow entropy computation paths

These are lower risk because:
- Unit, parser, policy, and publication paths are exercised by 1,317 tests
- YAML parsing is mostly libyaml's responsibility
- WebSocket inputs are text-based and already hit by the ASan runtime smoke test

### OSS-Fuzz (future)

Submitting `fuzz/fuzz_bless.c` to [OSS-Fuzz](https://github.com/google/oss-fuzz)
would provide continuous fuzzing on Google's infrastructure. The harness does
not depend on DPDK. Submission work is not currently scheduled.

---

## CI Enforcement

| Gate | Trigger | What it checks |
|------|---------|----------------|
| `make test` | Every push | 1,317 unit + integration tests |
| ASan + UBSan unit tests | Every push | `test_dist` + `test_core` under sanitizers |
| Deterministic seed test | Every push | `--seed` is accepted and logged |
| Runtime smoke test | Every push | Bless starts, HTTP server responds, workers produce entropy |

All gates are required -- any failure blocks the merge.

---

## Input Safety

Bless avoids `atoi()` / `atol()` in favour of `strtol()` with end-pointer validation.
All integer parsing in CLI overrides (`src/main.c`) and YAML config (`src/config.c`)
uses `strtol()` / `strtoul()` with explicit range checks.  The 1,154-unit-test suite
includes 280+ cases that verify reject paths for:

- Non-numeric input
- Negative values where unsigned is expected
- Values exceeding protocol maxima (e.g., port > 65535)
- Truncated YAML keys
- Empty arrays / empty range specs

---

## WebSocket Authentication

**Status**: optional, env-var driven.

When the environment variable `BLESS_API_KEY` is set to a string from 16
through 128 bytes, every WebSocket connection must provide the same key as a
query parameter in the upgrade URL:

```
ws://127.0.0.1:8000/wsURL?api_key=your-secret-key-here
```

- The comparison uses `CRYPTO_memcmp` (constant-time, from OpenSSL) to prevent
  timing side-channels.
- If `BLESS_API_KEY` is not set, all connections are accepted (backward
  compatible with earlier releases).
- Keys shorter than 16 bytes cause bless to **refuse to start** with a
  fatal error — no silent fallback to unauthenticated mode.
- Keys longer than 128 bytes also cause startup to fail.
- The home page enables Entropy and Observe only after a successful connection
  and opens them on that verified server origin. It transfers the in-memory key
  in a URL fragment, which the target page consumes and immediately removes
  from the address bar. The key is added to the WebSocket upgrade URL only
  because the browser WebSocket API cannot set an `Authorization` header.

### Connecting from `tools/www/index.html`

The home dashboard provides separate Server and API Key fields.  Fill both
before clicking **Connect**:

```text
Server:  ws://127.0.0.1:8000/wsURL
API Key: <BLESS_API_KEY>
```

Use the complete `ws://` or `wss://` URL when the HTML page and Bless use
different ports, for example when `index.html` is served on port 8080 and the
QEMU forwarding rule exposes Bless on port 8000.  A relative value such as
`/wsURL` uses the page's origin and would select port 8080.

When the page and WebSocket endpoint share an origin, the key can instead be
provided on the page URL:

```text
http://127.0.0.1:8000/?api_key=<BLESS_API_KEY>
```

The page copies this value into the API Key field and immediately removes it
from the address bar. The Server URL is retained in `localStorage`, but
`index.html` never persists the key. A page refresh therefore requires the key
again. Prefer entering the key in the API Key field; the query form exists for
backward compatibility.

Use a dedicated random `BLESS_API_KEY`. Never reuse a GitHub personal access
token, cloud credential, or another service's secret.

Prefer URL-safe random ASCII or hexadecimal keys; otherwise the query value
must be percent-encoded.  Bless reads `BLESS_API_KEY` only during server
startup, so restart it after changing the environment variable.

To generate a strong random key:

```bash
export BLESS_API_KEY="$(openssl rand -hex 32)"    # 64-char hex key
printf '%s' "$BLESS_API_KEY" | wc -c             # verify length
```

Set the variable in the same shell before starting BLESS. Do not commit the
value to Git, put it in a command-line argument, or print it in logs. Use the
same environment value when running the live control-plane smoke test:

```bash
export BLESS_CONTROL_ADDR=127.0.0.1:8000
make integration-live
```

The test checks that `/metrics` returns `200`, an invalid key cannot complete
the WebSocket upgrade, and the configured key can complete it. If
`BLESS_API_KEY` is absent, loopback WebSocket authentication remains disabled;
remote-control listeners are still refused.

---

## Known Gaps

1. **Fuzzing is not a required CI gate.**  The repository has fuzz targets,
   but pull-request CI does not build and run the required bounded corpus.
2. **No stack protector / FORTIFY_SOURCE.**  The release build does not set
   `-fstack-protector-strong` or `-D_FORTIFY_SOURCE=2`.  These are low-cost additions
   that should be added to the `release` Makefile target.
3. **Static analysis is limited to cppcheck.**  CodeQL, Coverity Scan, or
   `clang --analyze` are not integrated.
4. **WebSocket message handler not fuzzed.**  The message parsing is simple JSON
   (`{cmd, key, value}`) so the attack surface is small, but not formally verified.

---

## Reporting

Found a security issue?  Open a GitHub issue.  Unless `BLESS_API_KEY` is
configured, there is no authentication.  Standard public disclosure is fine.
