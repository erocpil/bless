# Remediation Roadmap

## Purpose

This roadmap converts the findings in
[`project-assessment-2026-07-28.md`](project-assessment-2026-07-28.md) into an
ordered implementation program.

The existing [`priority.md`](priority.md) remains the feature and data-path
optimization backlog.

This roadmap is authoritative for security, correctness, engineering gates,
deployment reliability, and release readiness.

## Guiding Rules

1. Correctness and safe defaults take precedence over new features.
2. Every behavior change requires a regression test.
3. Public control interfaces must fail closed.
4. Runtime behavior and reference documentation must change in the same commit.
5. Performance changes must preserve the benchmark methodology and report their
   measurement boundary.
6. Phase 2 does not begin until every Phase 1 exit criterion is satisfied.

## Three-Stage Program

| Stage | Objective | Main outcomes |
|-------|-----------|---------------|
| Phase 1 | Establish a safe and correct runtime baseline | Safe control plane, length-safe parsing, race-free runtime config, checked distribution arithmetic, safe statistics publication |
| Phase 2 | Make engineering verification and deployment repeatable | Required fuzz and concurrency gates, full integration coverage, hardened builds, reliable Docker workflows, documentation validation |
| Phase 3 | Improve architecture, release discipline, and sustainable performance | Reduced global coupling, stable extension contract, modular configuration, release automation, governance, measured data-path optimization |

## Timing Fidelity and Benchmark Environment Program

Bless's current software data path controls the number of packets submitted per
burst and the delay between bursts.  It does not guarantee the physical
transmit time of each packet.  The following ordered program makes that boundary
measurable before adding NIC-specific scheduling:

1. Add pre-flight environment validation with `warn`, `strict`, and `off`
   policies and a machine-readable environment snapshot.  Cover CPU governor,
   SMT sibling allocation, worker/NIC NUMA placement, CPU affinity, hugepages,
   NIC driver, and link state without modifying host settings.
2. State the software timing boundary in the README: rate and delay controls are
   batch-level, best-effort controls and do not promise nanosecond wire timing.
3. Publish software submission-timing quality metrics: target interval, sample
   count, late count, absolute-error percentiles, and maximum error.  Name and
   document these as host-to-PMD submission measurements, not wire timestamps.
4. Introduce a capability-driven pacing-backend interface with
   `software-batch`, `hardware-rate`, and `hardware-scheduled` implementations
   and an explicit fallback policy.
5. Prototype per-packet scheduled transmit on mlx5 hardware already represented
   in the benchmark environment; measure both timing error and throughput/WQE
   cost before enabling it by default anywhere.
6. Evaluate Intel ice/E810 independently through Traffic Management and TX
   scheduler capabilities.  Do not treat Dynamic Device Personalization (DDP),
   which primarily changes parsing and classification, as a scheduled-send API.

Acceptance requires preserving the selected backend, capability probe results,
pre-flight report, and timing-quality measurements with every benchmark result.
Hardware scheduling is not considered validated without NIC hardware timestamps
or an independent wire-time measurement.
The interface, time semantics, fallback behavior, and hardware validation plan
are specified in [`../design/pacing-backend.md`](../design/pacing-backend.md).

## Current Baseline

The status table above is the current Phase 1 source of truth; the detailed
R1-R8 sections below retain historical recommendations and are labeled where
their original state descriptions no longer apply.

The historical assessment remains useful, but several Phase 1 foundations have
landed since its assessed revision.

The table below is the current source of truth for Phase 1 progress.

| Task | Status | Implemented | Remaining |
|------|--------|-------------|-----------|
| P1.1 Control-plane security | Partial | API key authentication, constant-time comparison, fail-closed invalid key, DPDK-independent listener/query policy tests, explicit server.enable / control_enable / remote_control_enable switches, loopback-only default bind 127.0.0.1:8000, loopback HTTP/WebSocket auth fixture | Full BLESS server routing, key rotation, and non-loopback integration |
| P1.2 WebSocket framing | Core implementation complete | Explicit frame length, pre-allocation 4096-byte transport limit, FIN/text validation, embedded-NUL rejection, length-aware JSON parsing, structured per-connection errors, malformed-frame unit tests, dedicated fuzz_ws_frame target with 8-seed corpus | |
| P1.3 Runtime field schema | Core implementation complete | Descriptor table, type and range validation, startup-only classification, structured set/get responses | Keep API documentation generated from or checked against the descriptor table |
| P1.4 Runtime configuration concurrency | Partial | Atomic shadow fields, named runtime access paths, fixes for identified scalar races, production `runtime_control` module exercised by a DPDK-independent TSan gate, generation-boundary protocol design doc | Implementation when multi-field batch set command is added |
| P1.5 Distribution arithmetic | Core implementation complete | Checked integer distribution, defined power-of-two overflow behavior, explicit unlimited mode, expanded unit tests and fuzz interface, bounded fuzz CI gate (fuzz_dist, 30s) | |
| P1.6 Statistics publication | Partial | Checked serialization with valid-snapshot fallback, bounded private-payload copy tests, pin/retry reader lifetime protocol, production `stats_guard` TSan stress, timestamp renamed to tsc_cycles, single-client live loopback backpressure/disconnect, concurrent HTTP readers, synthetic xstats boundary fixtures, and WebSocket close/broadcast lifetime serialization | Multi-client capacity benchmarking and exhaustive hardware-specific xstats matrix |
| P1.7 Regression gates | Core implementation complete | GCC/Clang builds, cppcheck, unit tests, ASan/UBSan smoke with halt_on_error, deterministic replay, runtime smoke, dependency audit, 7-target fuzz gate (30s each incl. ws_frame), DPDK-independent TSan gate, production YAML parser regression test, fuzz corpora management README | |
| P1.8 Documentation alignment | Core implementation complete | Security, API, runtime and concurrency references, local `.md` link validation in CI, all 13 non-loopback config files corrected to 127.0.0.1 binds, testing-coverage.md with hw/sw boundary | |

Phase 1 is not complete while P1.1, P1.4, or P1.6 has remaining
acceptance criteria.

### Remaining work classified by verification domain

**Software-verifiable** — items that can be implemented with local
CivetWeb listeners, socket clients, synthetic snapshots, or TSan.  No
real NIC or DPDK device required:

|| Item | Task |
||------|------|
|| P1.1 | Auth boundary tests: key rotation, non-loopback rejection, invalid/empty key | Deferred; baseline loopback/fail-closed policy is release-blocking |
|| P1.6 | Slow-reader / disconnect-during-broadcast with local WS client | Baseline and connection-lifetime race covered; long-duration pressure deferred |
|| P1.6 | Max xstats serialization boundary with synthetic snapshots | Deferred; bounded payload safety is covered |
|| P1.6 | Concurrent HTTP + WebSocket reader stress under TSan | Deferred; unit-level TSan gate remains |

The generation boundary design is documented in
`docs/design/generation-boundary.md`; its implementation is gated by
a future multi-field batch `set` command.

**Hardware-pending** (requires real NIC / DPDK device):

|| Item | Task |
||------|------|
|| P1.1 | Runtime config integration (WS set/get on running workers with real packet I/O) |
|| P1.6 | Throughput benchmarking (Reference Ladder, bare-metal) |

When the software-verifiable items above are complete, Phase 1 enters
the **software-complete / hardware-validation-pending** state, and
Phase 2 can begin with a clear hardware verification checklist.

## Phase 1: Safe and Correct Runtime Baseline

### Phase objective

Phase 1 removes defects that can cause unauthorized control, undefined behavior,
incorrect runtime behavior, or out-of-bounds reads.

Phase 1 does not add new protocols or workload features.

### Phase completion definition

Phase 1 is complete only when:

- The shipped host default binds all HTTP and WebSocket endpoints to loopback.
- A remote control listener cannot be enabled accidentally.
- WebSocket input is parsed with an explicit length and bounded size.
- Every documented runtime-mutable field either takes effect safely or is
  rejected as startup-only.
- Runtime shared scalar access is data-race-free under the C memory model.
- Distribution fuzzing completes without sanitizer findings or abnormal
  execution time.
- Unlimited and finite packet-count modes have explicit tested semantics.
- Published JSON and metric lengths cannot exceed initialized buffer data.
- A reader cannot observe a statistics buffer while it is being reused.
- The Phase 1 test suite and documentation checks pass in CI.

## Phase 1 Work Breakdown

### P1.1: Define the control-plane security model

Assessment findings:

- A-01
- A-15

Implementation:

1. Add an explicit `server.enable` setting.
2. Add an explicit `server.control_enable` setting.
3. Keep observation endpoints and state-changing control independently
   configurable.
4. Change `conf/config.yaml` and templates to bind
   `127.0.0.1:8000` by default.
5. Remove the implicit TLS port from the default configuration unless a
   certificate is present and explicitly selected.
6. Reject non-loopback control listeners unless a remote-control opt-in is set.
7. Define one authentication mechanism for remote control.
8. Apply the same policy to the WebSocket endpoint and future HTTP control
   endpoints.
9. Redact secrets and authentication headers from logs.

Minimum acceptable remote-control policy:

- Local-only mode requires no credential.
- Remote mode requires an explicit configuration flag.
- Remote mode requires a configured bearer token or an equivalent reviewed
  authentication mechanism.
- Token comparison uses a constant-time comparison where practical.
- Missing or invalid credentials fail before the WebSocket connection is
  accepted.

Tests:

- Default configuration listens only on loopback.
- Control can be disabled while metrics remain enabled.
- A remote bind without explicit opt-in is rejected.
- Missing and invalid credentials are rejected.
- A valid credential permits an authorized command.
- Observation endpoints do not accidentally expose control behavior.

Acceptance criteria:

- An unauthenticated non-loopback client cannot change worker state.
- Security and API documentation describe the same defaults as the code.

Suggested change boundary:

- One design commit for configuration schema and policy.
- One implementation commit for server enforcement.
- One documentation and integration-test commit.

### P1.2: Make WebSocket framing length-safe

Assessment findings:

- A-02
- A-05

Implementation:

1. Define `BLESS_WS_COMMAND_MAX`.
2. Reject fragmented or oversized commands unless fragmentation is implemented
   deliberately.
3. Replace `%s` logging of frame data with a length-limited representation.
4. Replace `cJSON_Parse` with `cJSON_ParseWithLength`.
5. Reject embedded NUL data when the command protocol is defined as JSON text.
6. Return a structured error for invalid JSON instead of only logging it.
7. Check all allocations in connect and command handlers.
8. Avoid logging credentials or complete untrusted request headers.

Tests:

- Empty frame.
- One-byte frame.
- Valid frame without a trailing NUL.
- Truncated JSON.
- Embedded NUL.
- Oversized frame.
- Non-text opcode.
- Repeated malformed commands.
- Allocation-failure path where practical.

Acceptance criteria:

- ASan and UBSan report no issue for the malformed-frame corpus.
- Parser behavior depends only on `data` and `datasize`.

Suggested change boundary:

- One focused implementation and test commit.

### P1.3: Introduce a runtime field schema

Assessment findings:

- A-04
- A-05

Implementation:

1. Define a descriptor for every runtime field.
2. Record the field name, value type, minimum, maximum, mutability, and apply
   function.
3. Parse JSON numbers into an intermediate checked representation.
4. Reject NaN, infinity, fractions for integer fields, negative unsigned
   values, and values outside the field range.
5. Mark startup-only fields explicitly.
6. Return an error when a client tries to modify a startup-only field.
7. Generate or validate the API field table from the same descriptor list.

Initial mutability classification:

| Field | Phase 1 decision |
|-------|------------------|
| `pps_rate` | Runtime mutable |
| `sample_interval` | Runtime mutable ✓ (propagation path: `af85947`) |
| `entropy_target` | Runtime mutable — depends on P1.4 relaxed atomics for safe worker read |
| `entropy_adapt_gain` | Runtime mutable — depends on P1.4 relaxed atomics for safe worker read |
| `batch` | Startup-only unless buffer resizing is implemented |
| `batch_delay_us` | Runtime mutable only if every worker reads the live value |
| `batch_jitter_us` | Runtime mutable only if every worker reads the live value |
| `bps_rate` | Runtime mutable only after an explicit bucket synchronization path exists |
| `num` | Startup-only unless finite-count semantics are redesigned |
| `traffic_model` | Startup-only in Phase 1 |
| Handshake fields | Startup-only unless the handshake worker supplies explicit apply callbacks |
| `seed` | Startup-only or restart-required with an explicit response |

Tests:

- Minimum, maximum, just-below-minimum, and just-above-maximum for every field.
- Negative, fractional, infinity, and NaN inputs.
- Startup-only rejection.
- Runtime update followed by an observed worker-side effect.
- API `get` returns the applied value, not only the requested value.

Acceptance criteria:

- No runtime field silently acknowledges an update that workers do not apply.
- Runtime API documentation can be traced to the descriptor table.

Suggested change boundary:

- One schema commit.
- Separate commits for each group of apply callbacks.
- One generated/reference documentation commit.

### P1.4: Remove runtime configuration data races

Assessment findings:

- A-03
- A-04

Implementation:

1. Convert independently updated scalar fields to `_Atomic` types.
2. Use `memory_order_relaxed` where fields have no ordering dependency.
3. Use a generation counter or immutable snapshot for related fields.
4. Make worker access go through named load helpers.
5. Make control-plane updates go through named store/apply helpers.
6. Remove direct field writes from the WebSocket parser.
7. Document the ownership and memory order of every runtime field group.

The change must not place a mutex on the packet-construction hot path.

Tests:

- Repeated concurrent update/read stress test.
- Worker-side observation of monotonic generation changes.
- ThreadSanitizer test against a DPDK-independent runtime configuration model.
- Multi-field update behavior when a generation boundary is required.

Acceptance criteria:

- No known C data race remains in documented runtime fields.
- The concurrency reference no longer justifies undefined behavior through
  architecture-specific assumptions.
- Hot-path assembly for relaxed scalar loads does not regress materially on
  supported x86-64 builds.

Dependency:

- P1.3 must define field ownership and mutability first.

Suggested change boundary:

- One atomic field conversion commit.
- One grouped-update mechanism commit if required.
- One concurrency documentation and TSan-test commit.

### P1.5: Replace unsafe distribution arithmetic

Assessment findings:

- A-06
- A-07

Implementation:

1. Change `distribute` to return a status code.
2. Validate non-null pointers, `n > 0`, `total >= n`, and non-zero weight sum.
3. Accumulate weights with checked `uint64_t` arithmetic.
4. Replace floating-point proportional allocation with integer quotient and
   remainder arithmetic.
5. Bound the remainder assignment loop by the number of categories.
6. Define the overflow result of `make_power_of_2`.
7. Rename the helper to express its checked behavior if its signature changes.
8. Enter the finite quota path only when `num > 0`.
9. Define `-1` through a named unlimited constant.
10. Propagate errors to startup validation instead of calling `exit`.

Recommended allocation formula:

```text
base_i      = remaining * weight_i / weight_sum
remainder_i = remaining * weight_i % weight_sum
```

The multiplication must use a checked wider representation or a decomposition
that cannot overflow the supported range.

Tests:

- Zero categories.
- Zero total weight.
- One category.
- `total < n`.
- Maximum accepted weight.
- Sum near `UINT32_MAX`.
- Sum near `UINT64_MAX` if supported.
- `make_power_of_2` at `0`, `1`, `2^31`, `2^31+1`, and `UINT32_MAX`.
- Packet counts `-1`, `0`, `1`, `n-1`, `n`, and a large finite count.
- Invariant: output sum equals total.
- Invariant: enabled categories receive their defined minimum.
- Determinism for identical inputs.

Acceptance criteria:

- `fuzz_dist` completes at least 100,000 bounded runs under ASan and UBSan.
- `UBSAN_OPTIONS=halt_on_error=1` produces no failure.
- No input within the supported range causes an unbounded remainder loop.

Suggested change boundary:

- One arithmetic and unit-test commit.
- One `bless_set_dist` integration commit.

### P1.6: Make statistics publication memory-safe

Assessment findings:

- A-08
- A-09
- A-10

Implementation:

1. Add a checked serialization result containing status and actual byte count.
2. Treat truncation as an error.
3. Never store or publish a length greater than the initialized data.
4. Choose a snapshot lifetime model.
5. Copy or retain the snapshot before performing a blocking network write.
6. Rename `ts_ns` to `tsc_cycles` or perform a documented nanosecond
   conversion.
7. Keep JSON schema versioning explicit when field names change.
8. Remove duplicate JSON keys.

Preferred snapshot options, in order:

1. Immutable heap snapshot with atomic pointer publication and reference count.
2. Fixed snapshot pool with reader counts.
3. Copy-on-read into a per-request buffer if measured request frequency makes
   the cost acceptable.

Tests:

- JSON just below, exactly at, and above the buffer limit.
- Maximum supported port and xstats serialization.
- Slow reader while the producer publishes multiple generations.
- WebSocket disconnect during broadcast.
- Concurrent HTTP and WebSocket readers.
- Timestamp unit/schema verification.

Acceptance criteria:

- ASan and TSan report no snapshot publication issue.
- Content length matches the actual response body.
- No response includes bytes from an adjacent field or prior snapshot.

Suggested change boundary:

- One checked serialization commit.
- One snapshot-lifetime commit.
- One timestamp/schema commit.

### P1.7: Add Phase 1 regression gates

Assessment findings:

- A-11
- A-12
- A-13
- A-14

Implementation:

1. Add a fast fuzz build and bounded run to pull-request CI.
2. Include these fuzz targets in the PR smoke run:
   - `fuzz_dist` (distribution arithmetic, the highest-risk target)
   - `fuzz_bless` (packet construction)
   - A new WebSocket control-parser fuzz target (from P1.2)
   - Distribution regression corpora from P1.5.
3. Run unit tests with `UBSAN_OPTIONS=halt_on_error=1`.
4. Add the DPDK-independent ThreadSanitizer runtime-config test.
5. Add a Markdown local-link checker.
6. Report unit, stub, integration, runtime, and fuzz tests separately.
7. Make temporary test binaries build outside tracked source directories.

CI time policy:

- Pull request: short deterministic corpus and bounded fuzz smoke run.
- Main branch: longer fuzz run.
- Scheduled job: extended fuzzing and concurrency stress.

Acceptance criteria:

- Every Phase 1 defect has a regression test that fails on the assessed
  revision and passes after its fix.
- Reference-document link failures block CI.
- Test output is concise by default and verbose on failure.

Dependency:

- Individual regression tests should land with their fixes.
- The consolidated gate is the final Phase 1 integration task.

### P1.8: Align security, API, and runtime documentation

Assessment findings:

- A-14

Implementation:

1. Update `guides/security.md`.
2. Update `reference/api.md`.
3. Update `reference/concurrency.md`.
4. Update `guides/runtime.md`.
5. Correct the broken links in `overview.md`.
6. State the exact distinction between unit, stub, integration, and runtime
   coverage.
7. Remove or correct commands that do not have matching Make targets.
8. Document the hybrid static runtime dependency model.

Acceptance criteria:

- The documentation link checker passes.
- Every runtime field has one documented mutability classification.
- Security defaults in documentation, templates, and code are identical.

Dependency:

- Final wording follows P1.1 through P1.7.

## Phase 1 Execution Order

The recommended order minimizes overlapping edits and makes each safety property
testable before the next subsystem changes.

| Wave | Tasks | Reason |
|------|-------|--------|
| 1 | P1.5, P1.2 | Independent arithmetic and framing fixes with immediate sanitizer value |
| 2 | P1.3 | Defines the runtime field contract before concurrency changes |
| 3 | P1.4 | Implements the contract with race-free access |
| 4 | P1.1 | Enforces the control-plane exposure and authentication policy |
| 5 | P1.6 | Corrects statistics publication and schema behavior |
| 6 | P1.7, P1.8 | Consolidates gates and aligns all reference documentation |

P1.1 can proceed in parallel with P1.3 and P1.4 if the server configuration
schema and runtime field descriptor files do not overlap.

## Phase 1 Suggested Pull Request Set

| PR | Scope | Must include |
|----|-------|--------------|
| 1 | Distribution correctness | Checked integer algorithm, unlimited-mode fix, unit tests, fuzz regression |
| 2 | WebSocket framing | Length-safe parse/logging, size limit, malformed-frame tests |
| 3 | Runtime field schema | Field descriptors, type/range validation, startup-only rejection |
| 4 | Runtime concurrency | Relaxed atomics, apply helpers, TSan model test |
| 5 | Control-plane policy | Safe binds, enable switches, remote opt-in, authentication tests |
| 6 | Statistics safety | Checked lengths, snapshot lifetime, timestamp schema, concurrency tests |
| 7 | Phase 1 integration | CI gates, documentation links, API/security/concurrency updates |

Each PR should remain independently buildable and should not combine unrelated
feature work.

## Phase 1 Risk Register

| Risk | Impact | Mitigation |
|------|--------|------------|
| Configuration compatibility break | Existing remote setups stop starting | Emit a clear migration error and document explicit remote opt-in |
| Atomic field layout affects copied structs | Build or runtime corruption | Remove raw `memcpy` of atomic-containing structures and add compile-time checks |
| JSON schema rename breaks dashboards | Dashboard or external consumer failure | Increment schema version and provide a compatibility field for one release |
| Snapshot lifetime adds allocation cost | Control-plane overhead | Measure request rate and use a bounded pool if allocation is material |
| Integer distribution changes exact quotas | Replay mismatch | Define the algorithm, add golden vectors, and treat the change as a versioned behavior |
| Authentication adds secret handling | Credential exposure through logs/config | Redact logs, restrict file permissions, and document rotation |

## Remaining Implementation Plan

The following work packages turn the remaining findings into reviewable changes.

### R1: Finish the control-plane security policy

Historical design notes (superseded by the current Phase 1 table above):

- The built-in CivetWeb default is `127.0.0.1:8000`.
- The shipped `conf/config.yaml` still overrides it with
  `8000,8443s`, which may listen beyond loopback.
- `BLESS_API_KEY` is optional, so an unset key leaves WebSocket control
  unauthenticated.
- Observation endpoints and state-changing WebSocket commands are registered
  together whenever the server starts.
- There is no implemented `server.enable` or `server.control_enable` switch.

Recommended design:

1. Add `server.enable`, `server.control_enable`, and
   `server.remote_control_enable` to the parsed server schema.
2. Default `server.enable` to true only if backward compatibility requires it.
3. Default `server.control_enable` and `server.remote_control_enable` to false.
4. Ship `127.0.0.1:8000` as the host default and remove implicit TLS from the
   default configuration.
5. Register `/metrics`, `/api/stats`, and dashboards independently from the
   WebSocket command handler.
6. Reject a non-loopback control listener unless remote control is explicitly
   enabled and a valid API key is configured.
7. Keep API keys out of logs, persistent browser storage, process arguments,
   and generated diagnostics.
8. Provide a migration error that names the required opt-in when an existing
   remote configuration is rejected.

Suggested tests:

- Parse defaults and assert a loopback-only listener.
- Start with control disabled and verify that metrics remain available.
- Reject remote control with no opt-in.
- Reject remote control with no key, an empty key, a short key, and an invalid
  key.
- Accept a valid key and verify one authorized command.
- Verify that observation endpoints cannot execute control commands.
- Verify that logs and error responses do not contain the configured key.

Exit criteria:

- No shipped host configuration exposes unauthenticated state-changing
  control.
- Remote control requires two independent decisions: explicit remote opt-in and
  a valid credential.
- API and security references describe exactly the defaults enforced by code.

### R2: Make statistics publication length-safe and lifetime-safe

Historical design notes (superseded by the current Phase 1 table above):

- `worker_generate_stats()` stores the return value of `snprintf()` directly in
  `json_len`.
- A truncated `snprintf()` returns the required size, which can exceed the
  initialized bytes in the destination.
- HTTP and WebSocket readers hold a pointer into a two-slot buffer while the
  producer can reuse that slot after two publications.
- The internal `ts_ns` snapshot field contains TSC cycles even though its name
  implies nanoseconds.

Recommended implementation:

1. Introduce a serialization result containing status and actual bytes stored.
2. Treat an allocation failure or output larger than the configured maximum as
   a dropped snapshot, never as a partially valid response.
3. Set `json_len` and `metric_len` only after successful serialization and
   assert that each is smaller than its corresponding buffer.
4. Use a bounded copy-on-read model for Phase 1.
5. Copy the selected JSON or metric payload while holding a short publication
   lock, release the lock, and perform network writes from the private copy.
6. Keep blocking CivetWeb writes outside the publication lock.
7. Measure the copy cost before replacing this simple model with a reference
   counted pool or immutable heap snapshot.
8. Rename `ts_ns` to `tsc_cycles`, or convert it to nanoseconds using
   `rte_get_timer_hz()`.
9. If a public JSON field changes, increment the schema version and retain a
   compatibility field for one release when practical.

Suggested tests:

- Serialize immediately below, exactly at, and above each output limit.
- Populate the maximum supported port and xstats set.
- Hold a simulated reader across at least three producer publications.
- Disconnect a WebSocket client during a broadcast.
- Run concurrent HTTP stats, metrics, and WebSocket readers.
- Verify that `Content-Length` equals the bytes written.
- Verify the unit and monotonic behavior of every published timestamp.
- Run the snapshot stress test under ASan and TSan.

Exit criteria:

- No published length exceeds initialized data.
- A writer cannot modify storage that any reader still references.
- Timestamp field names, values, dashboards, and API documentation use the same
  unit.

### R3: Make verification gates mandatory

Historical design notes (superseded by the current Phase 1 table above):

- Pull-request CI builds with GCC and Clang and runs cppcheck, unit tests,
  ASan/UBSan smoke, deterministic replay, runtime smoke and dependency checks.
- Repository fuzz targets are documented but are not built or run by CI.
- No ThreadSanitizer job exercises the runtime field model.
- No CI job validates local Markdown links.
- The full-binary sanitizer smoke mainly exercises startup and version paths,
  not packet construction behavior.

Recommended implementation:

1. Define explicit `test-unit`, `test-integration`, `fuzz-smoke`, `test-tsan`,
   and `docs-check` targets.
2. Build temporary test binaries under `build/test/`, not under tracked source
   directories.
3. Extract WebSocket command parsing into a DPDK-independent function and add a
   libFuzzer target for it.
4. Explicitly reject frames without the WebSocket FIN bit unless deliberate
   fragment reassembly is implemented.
5. Run `fuzz_dist`, `fuzz_bless`, the WebSocket parser, and fixed regression
   corpora for a bounded number of iterations on each pull request.
6. Run at least 100,000 bounded distribution iterations on `main`.
7. Add scheduled extended fuzzing for all targets.
8. Run unit and fuzz tests with
   `UBSAN_OPTIONS=halt_on_error=1`.
9. Add a small DPDK-independent concurrency model and execute it with TSan.
10. Add DPDK mbuf-based packet-builder integration tests for representative
   IPv4, IPv6, VXLAN, IMIX, mutation, and fragmentation paths.
11. Add a repository-local Markdown link checker and make failures blocking.
12. Report unit, stub, integration, runtime, fuzz and concurrency results as
    separate CI steps.

Suggested CI time policy:

| Trigger | Required gates |
|---------|----------------|
| Pull request | Unit, static analysis, short ASan/UBSan, deterministic fuzz corpus, short fuzz smoke, documentation links |
| Push to `main` | Pull-request gates plus full build, runtime smoke, packet-builder integration, 100,000-run distribution fuzz |
| Scheduled | Extended fuzzing, TSan stress, dependency scan, container smoke and longer concurrency tests |

Exit criteria:

- Every previously confirmed Phase 1 defect has a regression test.
- Sanitizer, fuzz, TSan and documentation failures block the relevant workflow.
- Test names and documentation state which production objects are exercised.

### R4: Harden release builds

Recommended implementation:

1. Add `-fstack-protector-strong` and `-D_FORTIFY_SOURCE=2` to supported release
   builds.
2. Add PIE and linker hardening where supported:
   `-fPIE`, `-pie`, `-Wl,-z,relro`, `-Wl,-z,now`, and
   `-Wl,-z,noexecstack`.
3. Keep sanitizer and fuzz builds in separate flag profiles so hardening flags
   do not hide diagnostics.
4. Fail configuration with a clear message when a required hardening flag is
   unsupported by a declared release compiler.
5. Audit the resulting ELF with `readelf` in CI.
6. Record direct and transitive dynamic dependencies for the hybrid static
   build.

Suggested checks:

- GNU stack is non-executable.
- ELF type is PIE where the platform supports it.
- `GNU_RELRO` is present and immediate binding is enabled.
- Stack protector symbols are present in code paths with protected frames.
- Release binaries still pass deterministic replay and runtime smoke tests.

### R5: Make container workflows match their documentation

Dependencies:

- Complete R1 before exposing any container listener beyond loopback.

Recommended implementation:

1. Add a dedicated container configuration instead of reusing
   `config-ci.yaml`.
2. Bind the container listener to the container interface only for endpoints
   intended to be published.
3. Keep state-changing control disabled by default in the container image.
4. Add a Docker `HEALTHCHECK` that exercises a read-only endpoint.
5. Test the health endpoint from outside the container namespace.
6. Replace the current dual-container handshake description with an actual
   TAP, veth, host-network, or other verified DPDK-capable topology.
7. Add setup and teardown scripts that create and remove only named test
   interfaces.
8. Assert a non-zero established-handshake counter before declaring the
   Compose test successful.

Exit criteria:

- The documented host URL is reachable after the documented container command.
- The default image does not expose unauthenticated control.
- The dual-instance test proves packet exchange rather than only process
  startup.

### R6: Keep documentation synchronized

Required work:

1. Keep `overview.md` links relative to the file's location and validate them
   in CI.
2. Remove references to `server.enable` until it exists, or land the
   implementation and documentation together.
3. Update `priority.md` when features land.
4. Update the Phase status table and task matrix in the same commit as each
   completed work package.
5. Distinguish unit, stub, integration, runtime, fuzz and production-code
   sanitizer coverage.
6. Keep security hardening claims aligned with the actual compiler flags and CI
   tools.
7. Keep public timestamp names and units aligned with the serialized schema.
8. Generate or validate dashboard runtime-field labels, ranges and mutability
   from the same descriptor table used by the server.

Exit criteria:

- All repository-local links resolve.
- No documented configuration key is absent from the parser.
- No completed feature remains marked TODO.
- Coverage claims name the target and production objects they exercise.

### R7: Reduce structural testing barriers

Recommended order:

1. Add direct unit tests for `shannon_from_sorted()` and
   `latency_hist_percentile()`.
2. Move function definitions from `mutation.h` into domain-specific `.c`
   modules while retaining declarations in headers.
3. Split `config.c` by configuration domain.
4. Replace configuration-path `exit()` and `rte_exit()` calls with structured
   errors returned to the caller.
5. Split remaining large initialization and control functions only after
   regression coverage exists.
6. Introduce explicit ownership objects for global `base`, `bless_conf`, and
   snapshot state.

Exit criteria:

- Pure entropy algorithms can be tested without DPDK initialization.
- Configuration errors are recoverable and carry path-specific diagnostics.
- Mutation modules can be compiled and tested independently.
- Refactoring does not change deterministic replay output without an explicit
  versioned reason.

### R8: Establish a release and ownership process

Recommended implementation:

1. Define versioning, compatibility and support policies.
2. Add `CHANGELOG.md`, contribution guidance and `CODEOWNERS`.
3. Build release artifacts from a tagged, clean, reproducible source state.
4. Publish the documented container image from CI.
5. Generate an SBOM for binaries and container images.
6. Add dependency scanning, artifact checksums, provenance and signing.
7. Document the release rollback and security-fix process.

Exit criteria:

- A tagged release produces traceable binaries, checksums, an SBOM and a
  matching container image.
- Review ownership is explicit for control plane, data plane, build, security
  and documentation changes.
- Users can identify compatibility changes from one authoritative changelog.

## Recommended Execution Sequence

| Wave | Work packages | Completion signal |
|------|---------------|-------------------|
| A | R1 control-plane policy, R2 statistics safety | Phase 1 runtime baseline is safe under remote input and slow readers |
| B | R3 verification gates, R6 documentation synchronization | Phase 1 regressions and reference drift are blocking CI failures |
| C | R4 release hardening, R5 container workflows | Phase 2 build and deployment paths are repeatable |
| D | R7 structural work, R8 release process | Modules are independently testable and releases are traceable |
| E | Measured data-path optimization and optional features | Changes have retained raw results and do not weaken prior gates |

## Phase 2 Outline: Verification and Deployment

Phase 2 begins after the Phase 1 exit criteria pass.

Planned work:

- Full packet-builder integration tests using real DPDK mbuf pools.
- Required fuzz jobs for all targets and a scheduled extended run.
- Full-binary ASan, UBSan, and selected TSan execution.
- Release hardening flags and ELF property auditing.
- Reproducible dependency and compiler version policy.
- Correct Docker host access and container health checks.
- A real dual-instance TAP or veth handshake topology.
- Container publication workflow, SBOM generation, and dependency scanning.
- A single authoritative documentation index and clean local-link gate.

Detailed task definitions will be added after Phase 1 implementation clarifies
the stable runtime and control interfaces.

## Phase 3 Outline: Architecture, Release, and Sustainable Performance

Phase 3 addresses structural improvements after the safe baseline and
verification system are stable.

Planned work:

- Reduce reliance on global `base`, shared `bless_conf`, and recursive mutable
  configuration.
- Split configuration parsing by domain and replace process exits with
  structured errors.
- Split large worker, server, and mutation modules.
- Define whether extensions are a compile-time API or a versioned runtime ABI.
- Establish release versioning, changelog, compatibility, signing, and
  ownership policy.
- Add CODEOWNERS and code contribution guidance.
- Optimize packet construction using Reference Ladder measurements.
- Re-run multi-queue and physical-link benchmarks after correctness changes.
- Publish performance changes only with pinned configurations and retained raw
  data.

Detailed Phase 3 work should be derived from measured Phase 2 results rather
than from speculative optimization.

## Status Tracking

| Stage | Status |
|-------|--------|
| Phase 1 | In progress: P1.2, P1.3, P1.5, P1.7 and P1.8 core complete; P1.1, P1.4 and P1.6 remain partial |
| Phase 2 | Blocked on Phase 1 exit criteria; detailed recommendations recorded in R3-R6 |
| Phase 3 | Backlog defined; begin only after verification and deployment gates are stable |

Update this table and the individual task acceptance criteria in the same change
that completes a task.
