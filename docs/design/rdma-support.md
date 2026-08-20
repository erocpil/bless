# RDMA Support Design

## Purpose and boundary

Bless is a stateless DPDK packet generator.  Its value is in the mbuf
construction layer: entropy injection, per-field mutation, IMIX, checksum
offload, and byte-level control over every header.  This document defines how
Bless should — and should not — engage with RDMA.

"RDMA support" splits into three distinct questions, with different answers:

| Question | Answer |
|----------|--------|
| Generate RoCEv2 **packet streams** to stress a peer (NIC, switch, gateway) | **In scope** — a new protocol constructor, analogous to VXLAN |
| Drive RDMA **through the verbs API** (`ibv_post_send`) instead of `rte_eth_tx_burst` | **Out of scope** — conflicts with Bless's core value |
| Act as a workload generator for a system-under-test that happens to be RDMA | **In scope** — same as the first row, the peer is the DUT |

Bless generates the *packet shape* of RoCEv2, not RDMA *semantics*.  It is not
an RDMA endpoint: it does not run the QP state machine (INIT→RTR→RTS), does not
perform connection management, does not acknowledge, and does not retransmit.
A real RDMA peer validates ICRC, PSN continuity, and QP context; a Bless stream
that randomizes these fields is exercised as malformed or edge-case RDMA
traffic, which is a feature, not a limitation.

## Why not verbs

The verbs data path has memory semantics: the application posts a buffer, the
NIC DMAs it directly, and the HCA fills packet headers in hardware.  Bless's
entire capability set — entropy, mutation, IMIX, per-field construction — lives
in the mbuf construction layer.  Moving to `ibv_post_send` would remove that
layer and reduce Bless to "a program that sends RDMA buffers," which perftest
and qperf already do better.

Concretely, the verbs path would require:

- A `libibverbs` / `rdma-core` dependency.
- Protection domain, memory region, completion queue, and queue-pair object
  management, replacing the mbuf pool.
- A connection state machine, replacing the stateless burst loop.

None of this composes with `distribute()` → `bless_mbufs()` → mutation →
`rte_eth_tx_burst()`.  It is a different data path, and it is deliberately
left out.

## RoCEv2 packet layout

RoCEv2 carries the InfiniBand transport inside a UDP datagram on IPv4/IPv6:

```
+----------------+  Ethernet (14 B)
| IPv4 (20 B)    |  or IPv6 (40 B)
| UDP (8 B)      |  dst port 4791
| IB BTH (12 B)  |  Base Transport Header
| IB payload     |  RETH / AETH / IETH / atomic, opcode-dependent
| ICRC (4 B)     |  Invariant CRC over BTH + payload
+----------------+
```

This is structurally the same as VXLAN: an inner protocol wrapped in an outer
Eth+IP+UDP envelope.  VXLAN appends an 8-byte VNI header after UDP; RoCEv2
appends a 12-byte BTH plus a 4-byte ICRC.  The existing VXLAN implementation
(`bless_encap_vxlan` in `src/bless.c`) is the reference for how to add RoCEv2.

## BTH field definition

The IB Base Transport Header is 12 bytes, big-endian on the wire:

| Byte | Field | Width | Notes |
|------|-------|-------|-------|
| 0 | `opcode` | 8 | SEND, RDMA WRITE, RDMA READ req/rsp, ACK, etc. |
| 1 | `se` / `m` / `pad_count` / `tver` | 1 / 1 / 2 / 4 | solicited event, migration, pad, transport version |
| 2-3 | `partition_key` | 16 | 0xFFFF for "no partition" |
| 4 | `rsvd` | 8 | reserved, 0 |
| 5-7 | `dqpn` | 24 | destination QP number |
| 8 | `ack_req` | 7 + 1 pad | acknowledge request bit |
| 9-11 | `psn` | 24 | packet sequence number |

All fields must be configurable per packet so Bless can produce both well-formed
streams (monotonic PSN) and edge-case streams (PSN gaps, bad opcode, wrong
partition key).

## ICRC

The Invariant CRC is a 32-bit CRC computed over the BTH and the payload's
invariant fields, **excluding** mutable fields (VLAN, IP ECN, and the fields
covered by the standard VCRC).  It is not the Ethernet FCS and not `rte_ipv4_cksum`.

Implementation notes:

- RoCEv2 ICRC uses the same generator polynomial as IEEE 802.3 CRC-32
  (`0x04C11DB7`), but with a RoCE-specific seed and a specific field mask that
  zeroes mutable bits (notably the IP ECN bits) before hashing.
- Bless has **no existing CRC capability** (confirmed: no CRC/ICRC code in
  `src/` or `include/`).  A `crc32c`-style table-driven function must be added,
  e.g. `include/roce.h` + `src/roce.c`.
- The ICRC must be computed in the constructor on the hot path.  A table-driven
  implementation (~1 byte/cycle) is acceptable; hardware ICRC offload is not
  available on the software construction path.

Correctness is load-bearing: an RDMA peer silently drops a packet with a bad
ICRC (unlike TCP, which retransmits).  A Bless RoCEv2 stream with a wrong ICRC
produces zero observable effect on the peer, which makes the benchmark
meaningless.  ICRC correctness is a P0 acceptance criterion.

## Design: `proto_roce.c` constructor

Follow the existing extension-protocol pattern (`bless_plugin.h`,
`proto_udp.c`):

```c
static const struct bless_pkt_type proto_roce = {
    .name       = "roce",
    .ether_type = RTE_ETHER_TYPE_IPV4,
    .ip_proto   = 17,             /* UDP */
    .type_idx   = BLESS_AUTO_IDX,
    .construct  = bless_mbufs_roce,
};
static void __attribute__((constructor)) reg_roce(void) {
    bless_register_pkt_type(&proto_roce);
}
```

`bless_mbufs_roce()` builds, per packet:

1. Inner Ethernet header (same as `proto_udp.c`).
2. Inner IPv4/IPv6 header.
3. Inner UDP header with `dst_port = 4791`.
4. BTH (12 B) from configurable fields.
5. Payload (opcode-dependent: RETH for WRITE/READ, AETH for ACK, raw for SEND).
6. ICRC over BTH + payload.

Unlike VXLAN (which *prepends* an outer envelope around an existing inner
frame), RoCEv2 is a single-level packet where the "inner" protocol is the IB
transport.  The constructor fills the whole frame in one pass, so it is closer
to `proto_udp.c` than to `bless_encap_vxlan`.

## Configuration

Add a `bless.roce` section to the Cnode config tree, mirroring the existing
`bless.vxlan` structure:

```yaml
bless:
  roce:
    enable: true
    opcode: send                 # send | write | read-req | read-rsp | ack | atomic
    partition-key: 0xffff
    dqpn: 1+16                   # destination QP range syntax, as with ports
    psn: 0                       # base PSN; +1 per packet when monotonic
    psn-mode: monotonic          # monotonic | random | gap (inject gaps)
    icrc: valid                  # valid | corrupt (inject ICRC errors)
    payload-len: 64              # IB payload length
    ecn: 0                       # IP ECN/DSCP bits for DCQCN experiments
  ether:
    type:
      ipv4:
        src: 10.0.0.1+64
        dst: [ "192.168.1.1" ]
```

Key extension points relative to existing config:

- `psn-mode` is the first per-packet *sequence* field in Bless.  It needs a
  per-worker monotonic counter in the constructor state, which the current
  constructors do not have.  This is the one architectural addition beyond
  copying the VXLAN pattern.
- `icrc: corrupt` is the RDMA analogue of the existing `erroneous` mutation
  framework, but scoped to the transport CRC.  It can reuse the mutation
  plumbing rather than a separate switch.
- `ecn` exposes the IP `type_of_service` byte, currently hard-coded to 0 in
  `proto_udp.c` and `bless_encap_vxlan`.  This is required for DCQCN
  (Data Center Quantized Congestion Notification) experiments and is a small,
  independent change.

## ECN / DSCP support

RDMA congestion control (DCQCN) marks IP ECN bits on congested switches; PFC
uses priority-based pause.  To exercise these paths, Bless must set the IPv4
`type_of_service` byte and (for PFC) the VLAN priority.  Currently:

- `proto_udp.c:73` and `bless.c:106` hard-code `type_of_service = 0`.
- VLAN priority exists only inside `mutation_mac_vlan` as a mutation.

A `dscp` / `ecn` config field on the inner and outer IP headers is a
prerequisite for RDMA congestion testing and is listed here as a separate,
low-risk change that also benefits non-RDMA workloads.

## Non-goals (explicit)

- **No verbs data path.**  No `libibverbs`, no QP/CQ/MR, no connection state
  machine.  Bless stays a stateless DPDK packet generator.
- **No RDMA semantic peer.**  Bless does not complete RDMA READ/WRITE
  operations, does not ACK, does not participate in connection management.
- **No InfiniBand (non-converged) support.**  RoCEv2 only; raw IB link-layer
  framing is out of scope for the DPDK Ethernet path.

## Verification plan

1. **Unit** — ICRC against known test vectors (RFC / RoCE spec examples);
   BTH serialization byte-for-byte against a captured reference frame.
2. **Deterministic replay** — same `--seed` produces byte-identical RoCEv2
   streams (BTH + ICRC), verified with `tools/ci_det_check.py`.
3. **Peer validation** — send a well-formed RoCEv2 stream to a real RDMA peer
   and confirm the peer accepts it (ICRC valid, PSN monotonic, dqpn matching a
   pre-established QP).  This is the P0 acceptance gate.
4. **Edge-case streams** — corrupt ICRC, PSN gaps, bad opcode; confirm the
   peer drops them silently (documents the intended "malformed RDMA" use case).
5. **Performance** — Reference Ladder tier for RoCEv2 construction cost,
   following the clean A/B methodology in `benchmarks.md` §6 (shared inner
   config, matched window, 5 restarts).

## Relationship to existing docs

- Reference Ladder methodology (`benchmarks.md` §6): RoCEv2 becomes a new
  clean tier, measured against the fixed-UDP baseline.
- Protocol extension guide (`docs/overview.md` §4.2): `proto_roce.c` follows
  the documented registration pattern.
- Observability: RoCEv2 packets are tagged with `ip_proto=17` (UDP), so the
  existing entropy sampler treats them as UDP unless a dedicated type_idx is
  registered, in which case `bless_pkt_ip_proto` reports the new type.
