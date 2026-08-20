# Testing Coverage Boundaries

This document lists the checks present at the current revision and the areas
they do not cover. Consult the named test when an assertion changes.

## Software-verified (CI-gated, no hardware required)

| Component | Test suite | Sanitizer | CI gate |
|-----------|-----------|-----------|---------|
| Distribution arithmetic (`dist.c`) | 60 unit tests + fuzz | ASan, UBSan | CI: `make test` + fuzz gate |
| IP/port/IPv6 parsing (`parse.c`) | 27 + 1154 unit tests + fuzz | ASan, UBSan | CI: `make test` + fuzz gate |
| WS frame validation (`ws_frame.c`) | 8 unit tests + fuzz (8 seeds) | ASan, UBSan | CI: `make test` + fuzz gate |
| Runtime config concurrency (`runtime_control.c`) | TSan stress (1M iters, MW/MR) | TSan | CI: TSan gate |
| Stats snapshot guard (`stats_guard.c`) | TSan stress (via test_tsan_runtime) | TSan | CI: TSan gate |
| Control-plane policy | Loopback, remote opt-in, key and query policy unit tests | — | CI: `make test` |
| Stats publication boundary | Reader lifetime and bounded private-payload copy tests | — | CI: `make test` |
| YAML config parsing | Production parser unit tests + fuzz (30s CI) | ASan, UBSan | CI: unit and fuzz gates |
| Device classification | 17 unit tests | — | CI: `make test` |
| Docs link integrity | `tools/ci_doc_links.py` | — | CI: doc check |
| Static analysis | `cppcheck --enable=warning,performance,portability` | — | CI: cppcheck |
| Dependency audit | `readelf -d` NEEDED check | — | CI: dep audit |
| Build reproducibility | `SOURCE_DATE_EPOCH` smoke | — | CI: deterministic seed test |
| Production binary smoke | ASan + UBSan binary smoke | ASan, UBSan | CI: ASan/UBSan step |
| Throughput sanity | net_null PPS baseline (CI runner) | — | CI: perf step |

## Software-verifiable (not yet CI-gated)

These items can be tested with local socket clients, CivetWeb
listeners, or synthetic data — no real NIC or DPDK device required.
They are planned but not yet implemented as CI gates.

| Component | Test needed | Approach |
|-----------|------------|----------|
| Remote-control auth boundary | Policy unit tests plus loopback HTTP/WebSocket fixture covering invalid and valid key handshakes | Full BLESS server routing, key rotation, non-loopback listener |
| Slow-reader stats correctness | Loopback stream backpressure and client disconnect fixture | Real BLESS WS broadcast with multiple clients |
| Max xstats serialization boundary | Near-8 KiB synthetic xstats payload copy/termination test | Actual DPDK serializer and JSON/Prometheus parity |
| Concurrent HTTP+WS reader safety | Eight concurrent `/metrics`/`/api/stats` readers plus one held WebSocket client on the live listener | Full TSan run with the production BLESS worker and DPDK stats writer |

## Hardware-pending (requires real NIC / DPDK device)

| Component | Test needed | Reason not software-verifiable |
|-----------|------------|-------------------------------|
| Runtime config integration | WS set/get on running workers, CAS retry on entropy-adapt | Requires DPDK worker threads with real packet I/O |
| Throughput benchmarking | Reference Ladder methodology (ABAB, 5 restarts) | Requires dedicated bare-metal hardware with consistent NIC state |

## Not covered by any test suite

| Area | Reason |
|------|--------|
| TLS/HTTPS cert validation | TLS listener removed from default config; cert handling not yet designed |
| Docker multi-arch builds | Docker workflow smoke-tests amd64 only |
| Graceful shutdown interrupt handling | SIGINT/SIGTERM behaviour is best-effort, not a tested contract |
| OOM / memory exhaustion paths | `calloc`/`malloc` checks exist but OOM injection is not automated |
| DPDK device hotplug | Not a supported feature |

## How to read this document

- **"Software-verified" items** have automated CI gates that run on every
  push and PR.  When these fail, the defect is in the code under test —
  not in the test environment.

- **"Software-verifiable (not yet CI-gated)" items** can be implemented
  with local, DPDK-independent test fixtures.  They are the next
  priority for closing the software-side coverage gap.

- **"Hardware-pending" items** are designed and specified but require a
  DPDK-capable machine with real NICs.  They form a verification
  checklist to be executed before a production release.

- **"Not covered" items** are explicitly excluded from the current
  testing scope.  Additions require a test plan and a CI gate design.
