# Project Assessment: 2026-07-28

## Purpose

This document records the repository assessment performed on 2026-07-28.

The assessment covers product scope, architecture, correctness, security, testing,
performance engineering, documentation, build and deployment, and project
governance.

The remediation plan derived from this assessment is maintained in
[`remediation-roadmap.md`](remediation-roadmap.md).

## Assessed Revision

| Item | Value |
|------|-------|
| Branch | `main` |
| Commit | `b2b9a4f9dbcd332072eed7a1169ad29750b1fece` |
| Commit date | 2026-07-27 |
| Working tree before assessment | Clean |
| Working tree after assessment | Clean, except for the documentation changes that record this assessment |
| Submodules | Initialized at the revisions recorded by the superproject |

The assessment describes this exact revision.

Later fixes do not change the historical findings in this document.

## Executive Summary

Bless has a clear and differentiated purpose.

It treats protocol composition, entropy, timing, distribution, mutation, and
encapsulation as controlled workload dimensions instead of treating packet rate
as the only experimental variable.

The Reference Ladder, entropy analysis, benchmark methodology, and observability
stack give the project substantial value as a research and engineering platform.

The current implementation is not ready for exposure as a production service.

The main blockers are:

1. The effective default configuration can expose an unauthenticated WebSocket
   control plane on non-loopback interfaces.
2. Runtime configuration uses non-atomic concurrent access and has mismatches
   between documented and actual live-update behavior.
3. Fuzzing found undefined behavior in the distribution and power-of-two code.
4. WebSocket and statistics output paths do not consistently preserve explicit
   buffer lengths and reader lifetimes.
5. Docker and documentation examples contain behavior that does not match the
   implementation.

Bless should currently be treated as an experimental platform for isolated and
authorized environments.

## Scorecard

Scores are qualitative indicators, not release gates.

| Area | Score | Assessment |
|------|------:|------------|
| Product identity and design | 8/10 | Clear differentiation and strong experimental model |
| Architecture | 7/10 | Good control/data-path separation, but shared global state creates coupling |
| Feature completeness | 8/10 | Broad protocol, mutation, rate, handshake, and observation support |
| Correctness and robustness | 5/10 | Good baseline behavior, but fuzzing found real undefined behavior |
| Security | 3/10 | Unsafe effective defaults and unauthenticated runtime control |
| Testing and CI | 7/10 | Broad CI structure, but important control, fuzz, and concurrency gaps remain |
| Performance engineering | 7/10 | Strong methodology and transparent limitations |
| Documentation | 6/10 | Broad coverage, but implementation drift and broken links reduce reliability |
| Build, deployment, and release | 5/10 | Functional design, but dependency and container workflows need hardening |
| Maintenance and governance | 5/10 | Active development with a high single-maintainer dependency |
| Overall | 5.8/10 | Strong experimental platform, not production-ready |

## Verification Performed

### Repository and dependency state

- `git pull --ff-only` reported that `main` was already up to date.
- The three recorded submodules were initialized successfully.
- The final tracked working tree was clean before this document was added.

### Unit tests

The DPDK-independent unit tests were compiled into a temporary directory to
avoid changing repository artifacts.

| Test | Assertions | Result |
|------|-----------:|--------|
| `test_dist` | 35 | Pass |
| `test_core` | 1,154 | Pass |
| `test_ipv6` | 27 | Pass |
| `test_device` | 17 | Pass |
| Total | 1,233 | Pass |

### Sanitizers

The AddressSanitizer and UndefinedBehaviorSanitizer variants passed:

| Test | Assertions | Result |
|------|-----------:|--------|
| `test_dist_asan` | 35 | Pass |
| `test_core_asan` | 1,154 | Pass |
| `test_sanitize_extended_asan` | 46 | Pass |

LeakSanitizer could not run in the assessment container because the process
environment uses ptrace.

Leak detection was disabled for the rerun while address and undefined behavior
detection remained enabled.

### Fuzzing

The following targets completed 10,000 runs without a sanitizer failure:

- `fuzz_bless`
- `fuzz_ip_range`
- `fuzz_http_payload`
- `fuzz_dns_encode`

`fuzz_dist` found undefined behavior and an abnormal long-running input.

`fuzz_yaml` could not be built because the assessment environment did not have
`pkg-config` or the libyaml development headers.

### Full build limitation

A full DPDK build was not completed in the assessment environment.

The environment was missing:

- `pkg-config`
- `autoreconf`
- DPDK development metadata and headers
- `cppcheck`

The submodules were present and pinned correctly.

The build limitation is therefore an environment limitation and is not evidence
that the assessed revision fails to compile in its documented CI environment.

## Strengths

### Controlled workload model

Bless models workload properties explicitly and makes them reproducible through
configuration and an explicit `--seed` (the ordinary generation path reproduces
identical traffic with a fixed seed; the mutation path reads the TSC directly
and is not seed-covered).

This supports controlled A/B experiments that are difficult to construct with
a simple maximum-throughput traffic generator.

### Reference Ladder

The Reference Ladder introduces workload features incrementally.

This is a strong methodology for separating packet construction cost, protocol
cost, mutation cost, encapsulation cost, and observation cost.

### Protocol and workload breadth

The project includes IPv4, IPv6, ARP, ICMP, TCP, UDP, SCTP, DNS, HTTP, NTP,
QUIC, VXLAN, handshake workloads, rate controls, traffic models, and mutation
paths.

The compile-time registration interface also gives protocol-specific code a
consistent integration path.

### Observability

The project exposes:

- Shannon entropy
- min-entropy
- mutual information
- flow statistics
- rate power spectral density
- latency percentiles
- Prometheus metrics
- embedded dashboards
- profiling flame graphs

The breadth of this subsystem is a major project asset.

### Performance methodology

The benchmark documentation records invalidated and inconclusive experiments
instead of presenting them as valid results.

It also distinguishes software construction throughput, PMD behavior, physical
link conditions, and environmental constraints.

### CI structure

The CI workflow includes GCC and Clang builds, cppcheck, unit tests, sanitizer
tests, deterministic replay, a DPDK runtime smoke test, dependency auditing, and
a loose throughput health gate.

This is a good base for the additional gates defined in the remediation roadmap.

## Findings

### A-01: Effective default control-plane exposure

Severity: Critical.

The built-in server default binds to `127.0.0.1:8000`, but the default
configuration overrides it with `8000,8443s`.

See:

- [`include/server_options.def`](../../include/server_options.def)
- [`conf/config.yaml`](../../conf/config.yaml)

The WebSocket endpoint accepts state-changing commands without authentication.

Supported commands include `start`, `stop`, `exit`, and runtime `set`
operations.

See [`src/worker.c`](../../src/worker.c).

A port-only CivetWeb listener can bind beyond the loopback interface.

The effective default therefore does not match the local-only security claim in
the API documentation.

Required outcome:

- The shipped default must bind only to loopback.
- Read-only observation and state-changing control must be independently
  configurable.
- Remote control must require an explicit opt-in and authentication policy.

### A-02: WebSocket input length is not preserved

Severity: Critical.

The WebSocket callback receives `data` and `datasize`, but logs the data with
`%s` and passes it to `cJSON_Parse`.

See:

- [`src/server.c`](../../src/server.c)
- [`src/worker.c`](../../src/worker.c)

Both operations depend on NUL termination instead of the explicit frame length.

Required outcome:

- Enforce a maximum command frame size.
- Log with a length-limited format.
- Parse with `cJSON_ParseWithLength`.
- Add malformed, truncated, embedded-NUL, and oversized-frame tests.

### A-03: Runtime configuration has C data races

Severity: Critical.

The WebSocket thread writes shared `bless_conf` scalar fields while worker
threads read them through plain non-atomic C accesses.

The behavior is described in
[`concurrency.md`](concurrency.md) as a theoretical C11 data race.

It is undefined behavior in the C abstract machine, not only a theoretical
portability concern.

The x86 aligned-store argument does not make the C program data-race-free.

Required outcome:

- Use relaxed atomics for independently updated scalar fields.
- Use an immutable snapshot or generation mechanism for related field groups.
- Add a ThreadSanitizer-compatible control/worker test.

### A-04: Runtime API behavior does not match documentation

Severity: High.

The API accepts updates for fields such as `batch`, `batch_delay_us`,
`bps_rate`, and handshake parameters.

Several workers copy these fields during initialization and do not consume the
shared values again.

`pps_rate` has an explicit live synchronization path and `sample_interval` has
an explicit propagation path, but many other accepted updates do not have
equivalent behavior.

Required outcome:

- Define a field schema with type, range, mutability, and application callback.
- Reject startup-only fields during runtime.
- Add a test that observes the effect of every documented mutable field.

### A-05: Runtime numeric values are insufficiently validated

Severity: High.

WebSocket numeric values are cast directly from `double` to fixed-width integer
types.

Negative, fractional, non-finite, and out-of-range values can wrap or truncate.

Required outcome:

- Reject non-finite and fractional values for integer fields.
- Check every value before conversion.
- Reuse the same validation rules for YAML, CLI, and runtime control where
  practical.

### A-06: Distribution arithmetic contains undefined behavior

Severity: Critical.

The distribution implementation accumulates 32-bit weights into an
`unsigned int`.

An overflow can produce a zero or incorrect sum before division.

The floating-point proportional calculation can then produce infinity or NaN,
followed by an undefined conversion to `uint64_t`.

See [`src/dist.c`](../../src/dist.c).

The assessment fuzzer observed:

- A shift exponent of 32 in `make_power_of_2`.
- A NaN-to-`uint64_t` conversion in `distribute`.
- An input that continued for an abnormal amount of time after the sanitizer
  reports.

Required outcome:

- Replace the floating-point allocation path with checked integer arithmetic.
- Define and test overflow behavior for the next-power-of-two helper.
- Make invalid input an error return instead of terminating the process.

### A-07: Unlimited packet mode enters the finite distribution path

Severity: Critical.

`num == -1` represents an unlimited run.

`bless_set_dist` currently checks `if (bconf->num)` and passes the negative value
to an unsigned `total` parameter.

See [`src/bless.c`](../../src/bless.c).

Required outcome:

- Enter the finite quota path only when `num > 0`.
- Treat `num == -1` as unlimited through a named constant or explicit mode.
- Add regression tests for `-1`, `0`, small finite values, and maximum accepted
  finite values.

### A-08: Statistics JSON truncation can produce an invalid length

Severity: High.

The return value of `snprintf` is stored directly in `json_len`.

When truncation occurs, the return value is the required length, not the number
of bytes stored.

A subsequent WebSocket write can therefore be given a length larger than the
buffer.

See [`src/worker.c`](../../src/worker.c).

Required outcome:

- Detect truncation explicitly.
- Never publish a length greater than the initialized buffer content.
- Add a large-port/xstats serialization test.

### A-09: Statistics double buffering does not protect reader lifetime

Severity: High.

An atomic active-buffer index ensures that a reader selects a completed
snapshot.

It does not prevent the writer from reusing that buffer while a slow reader is
still sending it.

Required outcome:

- Use immutable reference-counted snapshots, reader tracking, or an equivalent
  lifetime mechanism.
- Keep all network writes outside the snapshot publication critical section.

### A-10: Statistics timestamp name and value disagree

Severity: Medium.

The `ts_ns` field is assigned `rte_get_tsc_cycles()`.

This is a cycle counter, not a nanosecond timestamp.

Required outcome:

- Rename the field to `tsc_cycles`, or convert it to nanoseconds with an
  explicitly documented epoch.
- Update the HTTP API schema and tests.

### A-11: Security hardening is incomplete

Severity: High.

The release flags do not explicitly enable stack protection, fortified libc
calls, or a complete linker hardening policy.

The project security guide already records part of this gap.

Required outcome:

- Add supported compiler and linker hardening flags.
- Audit the resulting ELF properties in CI.
- Document the remaining dynamic dependencies of the hybrid static build.

### A-12: Fuzzing is not a CI gate

Severity: High.

The repository contains six useful fuzz targets, but the main CI workflow does
not build or run them.

The distribution defect found during this assessment demonstrates the value of
making fuzzing a required gate.

Required outcome:

- Build all fuzz targets in CI.
- Run a bounded smoke corpus on every pull request.
- Run a longer scheduled fuzz job with sanitizer failures treated as test
  failures.

### A-13: Test coverage claims are broader than actual coverage

Severity: Medium.

The extended sanitizer test uses stubs and included helper implementations.

It does not provide full DPDK packet-builder integration coverage.

The security guide attributes 1,154 tests to packet construction paths, but
that count primarily comes from the core parsing and mathematical test.

Required outcome:

- Separate unit, stub, integration, and runtime test counts.
- Add DPDK mbuf-based packet construction tests.
- Keep documentation claims tied to named test targets.

### A-14: Documentation contains broken links and implementation drift

Severity: Medium.

The local link check found 23 broken links in `docs/overview.md`.

The links use a `docs/...` prefix even though the file is already inside
`docs/`.

Other observed drift includes:

- A documented server enable option that is not implemented.
- Runtime fields documented as mutable when workers do not apply them live.
- A `ts_ns` field documented as nanoseconds when it contains TSC cycles.
- Security text that says no static analysis exists even though CI runs
  cppcheck.
- Build and fuzz commands that do not exactly match available Make targets.
- A zero-runtime-dependency claim that conflicts with the audited hybrid static
  dependency model.

Required outcome:

- Add a repository-local documentation link checker.
- Update API and security documentation in the same change as implementation.
- Treat documentation drift as a CI failure for reference material.

### A-15: Docker smoke-test networking is inconsistent

Severity: High.

The Docker image defaults to `config-ci.yaml`, which binds the web server to
container loopback.

The Docker guide publishes port 8000 and expects it to be reachable from the
host.

Container port forwarding does not normally reach a service bound only to the
container loopback interface.

Required outcome:

- Provide a dedicated container smoke configuration that binds the intended
  container interface.
- Keep host and bare-metal defaults local-only.
- Add an automated container health check from outside the container.

### A-16: Docker Compose handshake topology does not match its description

Severity: High.

The Compose file says the two containers exchange traffic through a PCAP
loopback or veth path.

Each container has its own network namespace and its own loopback interface.

The Compose file does not create the described shared TAP or veth endpoint.

Required outcome:

- Define an explicit shared TAP, veth, host-network, or DPDK-capable topology.
- Verify an established handshake counter in an automated test.

### A-17: Release and governance processes are incomplete

Severity: Medium.

The repository has a high single-maintainer dependency and does not contain a
complete release, container publication, changelog, SBOM, or ownership process.

The documentation refers to a pre-built container image, but the repository
contains no matching image publication workflow.

Required outcome:

- Define version and release policy.
- Add release artifacts, container publication, SBOM generation, and signing.
- Add code contribution and review ownership documentation.

## Architecture and Maintainability Notes

The control-path/data-path separation is conceptually sound.

The implementation still depends heavily on the global `base`, shared
`bless_conf`, and the recursive `Cnode` configuration object.

Large files such as `config.c`, `worker.c`, and `mutation.h` combine multiple
responsibilities and make isolated tests harder.

The protocol extension interface is a compile-time registration system.

It should not be described as a stable runtime plugin ABI unless dynamic loading
and compatibility rules are added later.

These concerns belong primarily to Phase 3 of the remediation roadmap.

## Current Suitability

| Use case | Suitability |
|----------|-------------|
| Isolated workload experiments | Suitable with normal validation |
| Reproducible benchmark development | Suitable, subject to documented environment controls |
| Authorized robustness testing | Suitable in an isolated network |
| Local dashboard and metrics | Suitable when bound to loopback |
| Shared lab network control plane | Not suitable without Phase 1 controls |
| Internet-facing service | Not suitable |
| Production traffic management | Outside project scope and not suitable |

## Follow-Up

The three-stage work program and the detailed Phase 1 backlog are defined in
[`remediation-roadmap.md`](remediation-roadmap.md).
