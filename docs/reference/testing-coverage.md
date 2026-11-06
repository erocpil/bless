# Testing Coverage Boundaries

This document lists the checks present at the current revision and the areas
they do not cover. Consult the named test when an assertion changes.

## Automated checks configured in CI

These entries describe the workflow and linked sources, not results of a new
hardware run or an assertion that every related production path is covered.

| Component | Test or gate | Boundary |
|---|---|---|
| Distribution, address/port/IPv6 parsing | `make test`, ASan/UBSan helper tests, production fuzz targets | Inspect each test recipe's source list |
| YAML tree parser | `test_config_yaml`, production `fuzz_yaml` plus instrumented libyaml | Not all downstream Bless semantic configuration |
| Configuration values/nodes/files/arrays and protocol domains | `test_config_value`, `test_config_node`, `test_config_file`, `test_config_array`, `test_config_bless_domains` | Domain target links DPDK; it is not a real device startup |
| WS frame validation | `test_ws_frame`, ASan/UBSan and production fuzz target | Does not fuzz the JSON command handler |
| Runtime config and statistics guard | TSan stress links `runtime_control.c` and `stats_guard.c` | Multiple readers/writers, not complete DPDK-process TSan |
| Control-plane policy | `test_control_policy` | Listener/key/query helpers, not all live server routing |
| Statistics publication and sockets | `test_stats_publication`, `test_stats_socket` | Synthetic publication and socket fixtures |
| Device classification | `test_device` | Driver-name policy, not physical device validation |
| Entropy and sampling | `test_entropy_diag`, `test_entropy_math`, `test_entropy_sampler_policy` | Helper/model coverage, not all worker call paths |
| Timing and PSD | `test_timing_policy`, `test_rate_psd_ring` | Synthetic timing/ring behavior, not NIC departure timestamps |
| Process CPU metrics | `test_cpu_usage` | CPU accounting helpers |
| Documentation | `tools/ci_doc_links.py` | Local Markdown target paths; not semantic correctness or complete indexing |
| Production sanitizer build | Full ASan/UBSan binary, `--version` | No packet I/O in this sanitized smoke |
| Deterministic seed | Two production runs, `tools/ci_det_check.py` | Selected entropy values within tolerance; excludes time/hardware fields |
| Runtime / throughput sanity | Production `net_null` process, HTTP metrics | Detects missing worker activity; not a hardware benchmark |
| Static analysis / dependency audit | cppcheck, `readelf` and `ldd` | Configured warning categories and dependency allowlist |

The source of truth for unit targets is `UNIT_TESTS` in the root Makefile;
CI commands are in `.github/workflows/ci.yml`.
Historical assertion counts do not establish current coverage.
A failed gate fails its job; required merge checks are configured separately.

## Integration fixtures outside the default CI gate

| Fixture | Command | Coverage and remaining boundary |
|---|---|---|
| CivetWeb socket fixture | `make integration-test` | Isolated HTTP/WS routing, authentication and reader behavior; not the complete Bless worker |
| Live control plane | `make integration-live` | Requires an already running Bless listener; tests metrics and valid/invalid key upgrades |
| Virtio-user | `make integration-virtio-user` | Requires DPDK/vhost-net and privileges; can skip if backend unavailable |

These targets exist but are not invoked by the default CI workflow.
Further work includes complete live-process multi-client TSan, real DPDK xstats
serialization boundaries, key rotation/restart behavior and slow-client
broadcast tests with the production writer.

## Runtime and hardware validation

| Component | Test needed | Reason not software-verifiable |
|-----------|------------|-------------------------------|
| Runtime config integration | WS set/get on running workers, CAS retry on entropy-adapt | Requires running DPDK workers; virtual PMDs can cover software behavior, real NICs are needed for physical I/O effects |
| Throughput benchmarking | Reference Ladder methodology (ABAB, 5 restarts) | Requires dedicated bare-metal hardware with consistent NIC state |

## Not covered by any test suite

| Area | Reason |
|------|--------|
| TLS/HTTPS cert validation | Custom TLS listeners are configurable; certificate validation/rotation lacks a dedicated test |
| Docker multi-arch builds | Docker workflow smoke-tests amd64 only |
| Graceful shutdown interrupt handling | SIGINT/SIGTERM behaviour is best-effort, not a tested contract |
| OOM / memory exhaustion paths | `calloc`/`malloc` checks exist but OOM injection is not automated |
| General device removal/recovery | The Virtio-user fixture exercises device creation; it is not general live device removal/recovery coverage |

## How to read this document

A test establishes only the behavior it executes or asserts.
A sanitized binary build is not a sanitized packet-I/O run, and a software PMD
result is not a wire-throughput result.
Use the named test source and workflow command to decide whether an intended
change needs a new software test, an opt-in integration run or hardware evidence.
