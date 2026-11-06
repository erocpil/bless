# Phase 1 Follow-up Plan

This document is the execution checklist for the remaining Phase 1
software work. It complements `remediation-roadmap.md`: the roadmap
describes scope and history, while this file records the concrete
order, acceptance criteria, and verification required to close the
current gaps.

## Execution order

### 1. YAML parser termination and production fuzz coverage

Status: complete

Work:

- Stop parsing immediately after a nested libyaml parse failure.
- Add the malformed four-byte input `0a 0a 7d 0a` as a regression test.
- Move the generic YAML tree parser into a DPDK-independent production
  module.
- Make `fuzz_yaml` call that production module rather than maintaining
  a copied parser.
- Keep OSS-Fuzz and CI on the repository-pinned libyaml sources.

Acceptance:

- The regression input terminates without timeout.
- Valid mappings, sequences, nested structures, multiple documents,
  empty input, and malformed input have unit coverage.
- The fuzz target contains no copied YAML parsing implementation.
- ASan/UBSan fuzz smoke completes without a timeout or sanitizer error.

### 2. P1.1 control-plane security integration tests

Progress: the DPDK-independent loopback fixture now exercises HTTP routing,
invalid/valid WebSocket key handshakes, and is available through
`make integration-test` after `make -C third_party build-civetweb`.

Release posture: the loopback default, fail-closed remote-listener policy, and
API-key gate are the required control-plane baseline. Non-loopback matrices,
key rotation, and command-level live tests are deferred because they do not
block the core packet-generation path.

Status: in progress

Work:

- Extract DPDK-independent listener-policy and API-key decisions where
  necessary so they can be tested without starting packet workers.
- Test loopback and non-loopback listeners.
- Test `control_enable` and `remote_control_enable` combinations.
- Test missing, empty, short, invalid, URL-encoded, and valid API keys.
- Verify observation-only mode does not register state-changing
  WebSocket control.
- Verify diagnostics do not expose configured credentials.

Acceptance:

- Remote control requires explicit opt-in and a valid key.
- Observation endpoints remain independently available.
- All policy branches are exercised by a mandatory CI test.

### 3. P1.6 statistics publication boundary and concurrency tests

Progress: the fixture also exercises a non-reading client with a bounded
stream and an explicit disconnect; synthetic near-limit xstats payload tests
cover the private-copy boundary. WebSocket close and broadcast are serialized
so CivetWeb cannot free a connection while the broadcaster uses it. Full BLESS
multi-client pressure remains.

Release posture: snapshot lifetime safety, bounded copies, and disconnect
handling are required. Long-duration multi-client pressure, maximum DPDK
xstats, and full TSan network stress are deferred follow-up work.

Status: in progress

Work:

- Test readers held across at least three publications.
- Test slow-reader and disconnect-during-broadcast behavior with a
  controlled writer callback or local socket fixture.
- Test JSON and Prometheus output immediately below, at, and above
  their configured limits.
- Test maximum synthetic port/xstats serialization.
- Stress concurrent HTTP-style, metrics-style, and WebSocket-style
  readers under TSan.

Acceptance:

- Writers never reuse a snapshot while a reader pins it.
- Network writes occur from private copies after releasing the snapshot.
- Published lengths never exceed initialized bytes.
- Disconnect and short-write paths terminate cleanly.
- DPDK-independent boundary tests and TSan stress are CI-gated.

### 4. Documentation status reconciliation

Status: complete

Work:

- Remove or label historical R1-R3 “Current state” descriptions that
  conflict with the Phase 1 source-of-truth table.
- Reconcile the bottom phase-status table with completed P1.2, P1.3,
  P1.5, and P1.8 work.
- Update `testing-coverage.md`, the remediation roadmap, and this
  checklist in the same change as each completed work package.
- Keep the runtime-field API reference generated from or checked
  against the descriptor table.

Acceptance:

- A single current status is stated consistently across reference
  documents.
- No completed item remains marked pending.
- Documentation link validation remains green.

## Deferred or conditional work

- Multi-field runtime batch `set`: implement the generation-boundary
  design only when a batch command is introduced.
- Real worker runtime-control integration and Reference Ladder
  throughput validation require DPDK-capable hardware.
- Structural P3 work—splitting `config.c`, moving implementations out
  of `mutation.h`, replacing configuration-path process exits, and
  decomposing large functions—follows the Phase 1 verification work.
- Release hardening, container topology, SBOM, signing, CODEOWNERS, and
  publication workflows remain Phase 2/3 work.

## Hardware validation checklist

- Exercise authenticated WebSocket set/get against running workers and
  real packet I/O.
- Exercise entropy-adaptation compare/exchange conflicts under load.
- Run the Reference Ladder methodology on bare metal with a real NIC,
  ABAB ordering, and five process restarts.
