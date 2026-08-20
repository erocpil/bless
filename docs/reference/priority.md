# BLESS Code Optimization Priorities

> Priority ranking based on the entropy injection ladder.
> Updated after the Phase 1 follow-up work; reconcile this list with the
> current status table before starting a new work package.
> Check off items as completed.

---

## Audit Baseline: HEAD vs README

Deviations between README and HEAD, fixed in `84f5edd`:

| Deviation | Fix |
|------|------|
| "auto-adjusts PPS to sustain entropy_target" -- adaptive closed-loop not implemented | Removed, replaced with "PPS real-time adjustable" |
| "QUIC, IPv6, **GRE**, SCTP" -- GRE does not exist | Removed GRE |
| "13-d Shannon" -> actual 15 dimensions | Updated to 15 |
| "29+ anomaly mutations" -> actual 35+ | Updated to 35+ |
| docs table missing 16 files + double-pipe formatting error | Filled in, formatting fixed |

---

## P0 -- Data-Plane Correctness

The previous 7 P0 issues (atoi()/ICMP ident/icmp_seq_next/OLD_CODE/VXLAN offload, etc.) are all fixed. The 2 new items below were discovered in the 2026-07-20 config/mutation audit.

| # | Issue | Location | Status |
|---|-------|----------|--------|
| 0.0 | **`n <= limit` OOB write** — 4 sequence-parsing loops (generic/port/IPv4/VXLAN-VNI) write one element past `array[limit]` when YAML has exactly `limit+1` items. Attack surface: config-file controlled. | `config.c:988,1027,1122,1187` | ✅ 2026-07-20 |
| 0.1 | `entropy_extract_5tuple` only handles types 0-4 (ARP-SCTP). QUIC/IPv6/DNS etc. return `default: return` -> entropy is 0 for these protocols | `entropy.h:183-190` | ✅ 2026-07-22 |
| 0.2 | **VXLAN IPv4 src `sizeof(uint16_t)` → `uint32_t[]`** — `config_parse_sequence_ipv4_vni_to_array` called with step=2 but target is `uint32_t[1024]`. Writes overlap: element N overwrites upper 2 bytes of element N-1. | `config.c:1894` | ✅ 2026-07-20 |

---

## P1 -- Entropy Dimension Completeness

| # | Issue | Impact | Status |
|---|-------|--------|--------|
| 1.1 | `entropy_extract_5tuple` coverage for extension protocols (same change as P0.1) | QUIC/IPv6/DNS/HTTP/NTP protocols generate traffic but entropy is not observable | ✅ 2026-07-22 |
| 1.2 | VXLAN is the only general tunnel encapsulation; ESP exists as a mutation path, while GRE and IPIP are absent | Additional gateway decapsulation paths are not covered | On-demand |
| 1.3 | IP fragment injection | Exercises tiny, overlapping and out-of-order IPv4 fragment reassembly | ✅ `f51486c` |
| 1.4 | **Erroneous.class capacity missing** — `n_clas` (max 32) and `n_type` (max 1024) have no bounds check; YAML can OOB-write `Cnode.erroneous.clas[]` and `type[]`. | `config.c:1973,1977` | ✅ 2026-07-22 |
| 1.5 | **VXLAN-over-IPv6 inner checksum** — unconditionally casts inner frame to `rte_ipv4_hdr*`, reads wrong offset for IPv6 inner (next_proto_id → Hop Limit), computes IPv4 pseudo-header checksum on IPv6 payload. | `bless.c:211-222` | ✅ 2026-07-22 |

The required correctness and entropy-completeness items are complete.

Additional tunnel types remain on demand and require a concrete test target
before implementation.

---

## P2 -- Traffic Statistical Properties

| # | Issue | Current | Fix | Status |
|---|-------|---------|-----|--------|
| 2.1 | IMIX packet-size distribution | Configured sizes are sampled by ICMP, TCP, UDP and SCTP constructors | Keep size selection bounded by MTU and covered by packet-builder integration tests | ✅ verified 2026-07-28 |
| 2.2 | `batch_jitter_us` application | The worker reads the configured value | Applied through `random_delay_jitter()` | ✅ verified |
| 2.3 | Reorder/retransmit injection | None | Configurable ratio swap adjacent packets + dup retransmit | Long-term |
| 2.4 | IPv6 `address+N` range support | Parsed ranges are retained in `src_range` and `dst_range` | The IPv6 constructor consumes each range when selecting addresses | ✅ `a5bc409` |
| 2.5 | Extended ASan test uses local helper implementations | `test_sanitize_extended.c` does not link all production modules | The limitation is explicit and CI adds a production-binary ASan smoke build | ✅ 2026-07-22 |

**Measured ceiling**:

| Baseline | MPPS | Notes |
|------|------|------|
| testpmd txonly | **46.35** | DPDK physical limit (zero construction, static mbuf infinite loop) |
| bless pktgen-equiv | 2.92 | Full Eth+IP+UDP+checksum (6.3% efficiency) |
| bless multi-q=8 | 5.40 | 8 cores x mixed protocols, doorbell saturated |

Breaking the ~5.4 MPPS bottleneck requires optimizing the construction path (pre-computed templates / batch fill), not adding more queues.

---

## P3 -- Architecture / Maintainability

| # | Issue | Location | Status |
|---|-------|----------|--------|
| 3.1 | `entropy_extract_5tuple` hardcodes 5-protocol switch | `entropy.h:183-190` | Same as P0.1 |
| 3.2 | Entropy algorithm pure functions lack unit tests (`shannon_from_sorted`, `latency_hist_percentile`) | `entropy.h` | **TODO** |
| 3.3 | `bless_parse_cfg_fields` 546-line giant function (7 type dispatches) | `bless_plugin.c:257-384` | Low priority |
| 3.4 | `token_bucket` no thread-safety annotations | `token_bucket.h` | Low priority |
| 3.5 | `mutation.h` contains non-static function definitions and must have exactly one translation-unit owner through `erroneous.h` | `include/mutation.h`, `src/config_bless_extra.c` | **TODO** |
| 3.6 | Split monolithic configuration parsing by responsibility and protocol domain | `src/config*.c` | ✅ File/Node/value, Ether/IP, VXLAN, erroneous/plugin and debug code split out (2026-08-03) |
| 3.7 | Configuration code used process-level `exit()`/`rte_exit()`, preventing error recovery and safe reuse | `src/config*.c` | ✅ Configuration and clone paths return errors (2026-08-03) |
| 3.8 | Large functions: `parse_args` (~300L), `base_init_topo` (~430L), `init_port` (~250L), `config_parse_bless_ether` (~520L), `config_parse_bless_vxlan` (~350L), `ws_user_func` (~150L) | `main.c`, `config.c`, `worker.c` | **TODO** |
| 3.9 | Duplicate VNI parse block extracted to `parse_vni_with_range()` | `config.c:1047` | ✅ 2026-07-20 |

---

## Historical Fixes (Closed)

The following P0-P3 items were all fixed in the previous audit cycle; retained for traceability:

<details>
<summary>P0 fixed (9 items)</summary>

| # | Issue |
|---|-------|
| 1 | Handshake worker exit path leak (10 MB hash table + mbuf arrays) |
| 2 | `bless_encap_vxlan()` runtime `rte_exit` on prepend failure |
| 3 | `bless_alloc_mbufs()` runtime `rte_exit` on allocation failure |
| 4 | `bless_create_pktmbuf_pool()` runtime `rte_exit` x2 |
| 5 | `bless_set_dist()` runtime `rte_exit` x3 |
| 6 | tx_only mbuf leak after construction failure |
| 7 | config.c YAML parse failure path memory leak |
| 8 | `mutation_arp_src` static `flag` thread-unsafe |
| 9 | worker.c handshake duplicate `cps_start_tsc` assignment |

</details>

<details>
<summary>P1-P3 fixed / noted</summary>

| # | Issue | Disposition |
|---|-------|-------------|
| 10 | `find_mutation_func()` in header, not `static` | Fixed |
| 11 | `*_mutators[]` arrays in `mutation.h` | Noted (WARNING comment exists) |
| 12 | Low unit test coverage | Fixed (+ test_core.c 1154 tests) |
| 13 | `entropy_samplers[]` hardcoded `RTE_MAX_LCORE` | Noted (standard DPDK) |
| 14 | ~47 `rte_exit` on startup chain | Noted (DPDK init convention) |
| 15 | `mutation_mac_multicast` fall-through | Fixed |
| 16 | `worker_func_flow()` empty stub | Noted (design intent) |
| 17 | ICMP missing TSC timestamp | Fixed |
| 18 | TCP missing TSC timestamp | Fixed |
| 19 | SCTP mutation missing | Fixed |
| 20 | IPv6 protocol builder missing | Fixed (proto_ipv6.c) |
| 21 | DNS/NTP/HTTP protocols missing | Fixed (proto_ + mutation_) |
| 22 | Poisson/self-similar model missing | Fixed (traffic_model=2 Pareto) |
| 23 | `exp_random()` no unit tests | Fixed |
| 24 | Inconsistent comment style | Fixed |
| 25 | `.gitignore` additions | Fixed |
| 26 | Doxygen comment coverage | Fixed |

</details>
