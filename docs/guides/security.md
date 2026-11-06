# Security

Bless is written in C and sends untrusted traffic patterns onto the wire -- memory safety
is a first-class concern, not an afterthought.  This document describes the sanitizer
coverage, fuzzing strategy, and CI enforcement in place.

---

## Verification scope

The repository's [CI workflow](../../.github/workflows/ci.yml) runs on pushes
and pull requests to `main`.
The checks below describe configured coverage, not a claim that every execution
path is safe or that the latest remote CI run passed.

| Check | What it exercises | Boundary |
|---|---|---|
| `make test` | Production parser, distribution, control policy, statistics, timing and entropy helper tests | Some older tests contain local implementations; inspect each target's linked sources |
| ASan + UBSan | Distribution, core parsers, WS frame validation and an extended helper suite | The extended suite includes local copies of DPDK-dependent logic |
| Sanitized production binary | Builds the full binary and runs `--version` | Does not execute packet transmission or worker allocation failure paths |
| Bounded fuzzing | Seven targets, each with a 30-second CI budget | Production and copied implementations are distinguished below |
| TSan | Production `runtime_control.c` and `stats_guard.c`, multiple readers/writers | Does not run the complete DPDK process under TSan |
| Deterministic seed | Two runs compared by `tools/ci_det_check.py` | Timing/hardware fields excluded; selected entropy values allow sampling-boundary tolerance |
| Runtime smoke | `net_null` worker activity and HTTP metrics | Functional check, not NIC throughput validation |
| Static analysis | cppcheck warnings, performance and portability checks | Not a proof of correctness |

Failures fail the corresponding workflow job.
Whether a job is required for merging is a repository branch-protection setting,
not something the workflow file alone establishes.
See [Testing Coverage Boundaries](../reference/testing-coverage.md) for remaining work.

### Running locally

Run unit tests from the repository root with initialized submodules:

```bash
make test
```

On a Linux host with the build dependencies installed, the production sanitizer
build and version smoke used by CI are:

```bash
make ASAN=1 O=build/asan-static
./build/asan-static/bin/bless --version
```

Use the explicit sanitizer compilation commands in the CI workflow to reproduce
its helper tests; setting `ASAN=1` does not by itself instrument every unit-test
recipe in the top-level Makefile.

## Fuzzing

The repository contains seven libFuzzer targets and an OSS-Fuzz-compatible build
script.
Local build support does not establish acceptance into the external OSS-Fuzz
service; no external enrollment or service status is asserted here.

| Target | Implementation under test |
|---|---|
| `fuzz_yaml` | Production `config_yaml.c` and repository-pinned libyaml, including tree construction and cleanup |
| `fuzz_dist` | Production `dist.c` |
| `fuzz_ip_range` | Production `parse.c`, including IPv6 range parsing |
| `fuzz_ws_frame` | Production `ws_frame.c` frame validation |
| `fuzz_http_payload` | Local HTTP builder/mutation helpers; not the production mbuf constructor |
| `fuzz_dns_encode` | Local DNS encoding helpers; not the production mbuf constructor |
| `fuzz_bless` | Production `make_power_of_2()` plus local checksum/IP/port helper copies |

YAML tree fuzzing does not exercise all downstream Bless configuration semantics
or protocol-domain parsing.
WS frame fuzzing does not exercise the complete JSON command handler or a live
CivetWeb connection.
Packet constructors, mutation dispatch and VXLAN need separate integration
coverage; text input alone does not make these paths low risk.

Use the [fuzzing build and run instructions](../reference/fuzzing.md#local-fuzzing).
There is no top-level `make fuzz` target at this revision.

## Input Safety

Validation belongs to the parser handling each input boundary.
Runtime numeric fields use the type/range descriptors in `src/runtime_field.c`;
YAML scalar helpers live in `src/config_value.c`, and address/port parsers in
`src/parse.c`.
Do not infer that every CLI and YAML field uses the same validation: for example,
some CLI floating-point options still use `atof()` in `src/main.c`.
Reject-path tests and fuzzing cover named modules, not every possible input path.

---

## WebSocket Authentication

WebSocket control is registered only when `system.server.control_enable` is true.
The default listener is loopback-only.
Non-loopback listeners require `remote_control_enable: true` and a configured
valid API key, even if the server is otherwise observation-only.
HTTP observation endpoints such as `/metrics` and `/api/stats` are not protected
by the WebSocket key; see the [API security switches](../reference/api.md#control-plane-security-switches).

When the environment variable `BLESS_API_KEY` is set to a string from 16
through 255 bytes, every WebSocket connection must provide the same key as a
query parameter in the upgrade URL:

```
ws://127.0.0.1:8000/wsURL?api_key=your-secret-key-here
```

- `control_policy_key_equal()` checks equal-length keys by accumulating byte
  differences; it does not call OpenSSL `CRYPTO_memcmp`.
- If `BLESS_API_KEY` is absent, enabled loopback WebSocket control does not
  require a key. An explicitly empty value is rejected at startup.
- Keys shorter than 16 bytes cause bless to **refuse to start** with a
  fatal error — no silent fallback to unauthenticated mode.
- Keys longer than 255 bytes also cause startup to fail.
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

- The full JSON command handler and DPDK packet paths are not covered by the
  standalone frame/parser fuzz targets.
- Release compiler flags do not explicitly enable `-fstack-protector-strong`
  or `_FORTIFY_SOURCE`; dependency/toolchain defaults are not a project-wide guarantee.
- Static analysis currently uses cppcheck; no CodeQL or Coverity workflow is present.
- OOM injection, TLS certificate handling and complete live-process concurrency
  need additional validation; see the coverage reference.

## Reporting

Follow the repository's [security reporting policy](../../.github/SECURITY.md).
Report vulnerabilities privately through the advisory process described there;
do not treat the absence of authentication as permission for public disclosure.
