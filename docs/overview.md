# Overview

Bless documentation is organized in five layers, read in order.

| Layer | Document | Question |
|-------|----------|----------|
| Identity | `README.md` | What is Bless and why does it exist? |
| Architecture | `docs/overview.md` (this document) | How does Bless work and why these design choices? |
| Theory | `docs/concepts/entropy-theory.md` | What is the scientific basis? |
| Methodology | `docs/reference/benchmarks.md` | How do we measure and validate performance? |
| Reference | `docs/*.md` | How do I use, build, extend, and verify Bless? |

Architecture before theory: understand the system first, then the principles
behind it.

### Documentation

| Goal | Document |
|------|----------|
| Understand the theory | [`concepts/entropy-theory.md`](concepts/entropy-theory.md) |
| Study the benchmark methodology | [`reference/benchmarks.md`](reference/benchmarks.md) |
| Configure a workload | [`guides/config.md`](guides/config.md) |
| Build from source | [`reference/BUILDING.md`](reference/BUILDING.md) |
| Publish the public snapshot | [`reference/repository-publication.md`](reference/repository-publication.md) |
| Use the API | [`reference/api.md`](reference/api.md) |
| Review the 2026-07-28 project assessment | [`reference/project-assessment-2026-07-28.md`](reference/project-assessment-2026-07-28.md) |
| Review the 2026-08-03 live observability validation | [`reference/observability-validation-2026-08-03.md`](reference/observability-validation-2026-08-03.md) |
| Follow the remediation roadmap | [`reference/remediation-roadmap.md`](reference/remediation-roadmap.md) |
| Execute the remaining Phase 1 work | [`reference/phase1-follow-up-plan.md`](reference/phase1-follow-up-plan.md) |
| See every documented topic | Full index below |

---


## 1. Mission

Traditional network stress-testing tools measure throughput -- maximum PPS,
maximum Mbps, minimum latency.  These metrics answer "how much traffic can
the gateway handle," but they cannot answer a more fundamental question:

**"How closely does the test traffic resemble real-world traffic?"**

A generator sending identical packets in a fixed pattern can saturate the
line rate, but the gateway under test triggers hardware pattern caches,
flow-table hit optimizations, and interrupt coalescing -- producing numbers
far higher than real-world deployment performance.  When the same gateway
faces genuine Internet traffic, the 5-tuple of each flow, packet size
distribution, and inter-arrival gaps all exhibit high-entropy
characteristics: low cache hit rates, high state-table pressure, frequent
interrupts.

**Bless exists to bridge this gap.**  It is a controllable workload
synthesizer that treats entropy as a first-class dimension of traffic
generation.  Where traditional tools ask "how fast," Bless also asks "how
realistic."  It quantifies traffic randomness as measurable entropy metrics
and puts those metrics on equal footing with PPS and latency.

Bless's core capabilities:

- **Controlled entropy injection** -- 15 Shannon dimensions + 12 min-entropy
  dimensions + 12 mutual-information pairs, all measured in real time.
- **Feature-isolated benchmarking** -- A reference ladder methodology that
  attributes the cost of individual features (VXLAN, mutation, distribution)
  through clean A/B pairs (shared inner config, single-variable diff); tiers
  that change the workload shape are cross-config and not treated as
  single-feature costs.
- **Anomaly injection** -- 50 mutation functions across 13 protocol
  categories, with per-packet ratio control.
- **Closed-loop adaptation** -- A P controller that adjusts packet rate to
  maintain a target entropy level, enabling self-regulating stress tests.
- **Hybrid static binary** -- DPDK and most dependencies are linked statically,
  with a small audited set of system runtime libraries.

For the theoretical foundations behind these capabilities, see
`docs/concepts/entropy-theory.md`.

---

## 2. Architecture

### 2.1 Subsystem Relationships

```
                                       +------------------+
                                       |   YAML Config     |
                                       |   CLI Args         |
                                       +--------+---------+
                                                |
                                       +--------+---------+
                                       |  Cnode Config Hub  |
                                       |  (config struct    |
                                       |   tree)            |
                                       +--------+---------+
                                                |
             +----------------------------------+----------------------------------+
             |                                                                     |
    +--------+---------+                                               +----------+---------+
    |  Main Lcore      |                                               |  Worker Lcore x N  |
    |  (control/stats)  |          lock-free ring                       |  (tx/rx)           |
    |                   |<----------------------------------------------+                   |
    |  compute_entropy  |  entropy_sampler                              |  bless_mbufs()    |
    |  flow_entropy     |  (per-worker, lock-free)                      |  protocol builder  |
    |  adapt_pps_rate   |                                               |  VXLAN encap       |
    |  stats_broadcast  |                                               |  anomaly injection |
    +--------+---------+                                               +--------+----------+
             |                                                                  |
             |  WebSocket broadcast (3 Hz)                                      |
             v                                                                  v
    +--------+---------+                                               +--------+----------+
    |  civetweb HTTP/WS |                                              |  NIC / PCAP PMD   |
    |  /dashboard       |                                              |  rte_eth_tx_burst |
    |  /entropy         |                                              +-------------------+
    |  /metrics         |
    +-------------------+
```

### 2.2 Core Data Flow

**Config Layer -> Config Hub.**  YAML file + CLI args are parsed by
`config_init()` / `parse_and_merge_config()` into a `struct Cnode` tree.
Cnode is a recursive struct covering MAC / IP / port ranges, protocol weights,
VXLAN tunnel parameters, anomaly injection config, etc.  The `base` global
singleton holds `bconf` (bless_conf) and `config` (config wrapper).  All
workers read current config via `bconf->cnode`.

**Config Hub -> Worker.**  On startup, each worker reads construction
parameters from `bconf->cnode` and builds a weight distribution table `dist`.

**Worker -> NIC.**  Worker main loop `worker_func_tx_only()`:

1. Allocate mbuf from pool
2. `distribute()` randomly selects protocol type by weight
3. `bless_mbufs()` dispatches to the per-protocol builder
4. If VXLAN encapsulation is needed: `bless_encap_vxlan()` wraps outer headers
5. If anomaly injection is needed: apply mutations at `Cnode.erroneous.ratio`
   probability
6. `rte_eth_tx_burst()` sends

**Worker -> Main Lcore (Entropy Feedback).**  Each worker maintains a
lock-free `entropy_sampler` array.  After sending each batch, the worker
atomically updates counters.  Main lcore polls all worker samplers every 100 ms
and computes entropy values every 1-10 s (Shannon H, Min-entropy, Mutual
Information).

**Main Lcore -> WebUI.**  Main lcore publishes stats and entropy data via
double-buffering.  The WebSocket thread broadcasts to the browser dashboard
every 333 ms (3 Hz).

### 2.3 Worker Model

| Mode | Function | Behavior |
|------|----------|----------|
| tx-only | `worker_func_tx_only` | Build packets -> tx_burst (default) |
| rx-only | `worker_func_rx_only` | rx_burst -> free |
| fwd | `worker_func_fwd` | RX -> swap MAC -> TX |
| flow | `worker_func_flow` | Reserved |
| handshake | `worker_func_handshake` | TCP SYN/SYN-ACK/ACK state machine (see 3.8) |

State machine: `STATE_INIT -> STATE_RUNNING -> STATE_STOPPED -> STATE_EXIT`,
synchronized across cores via `atomic_int g_state`.

### 2.4 Control Plane

Embedded HTTP/WebSocket service based on civetweb (separate thread).

| Endpoint | Purpose |
|----------|---------|
| `/` | Web dashboard |
| `/entropy` | Multi-dimensional entropy panel |
| `/observe` | Real-time traffic observation |
| `/metrics` | Prometheus metrics (DPDK port stats + entropy metrics) |
| `/api/control` | Runtime configuration changes |
| `/api/stats` | Runtime statistics |
| `/wsURL` | WebSocket real-time broadcast |

### 2.5 Cnode Config Tree

`Cnode` is the central configuration data structure -- a recursive tree that
represents every aspect of the packet recipe.  All subsystems (YAML parser, CLI
overrides, worker packet construction, entropy max computation) read from the
same Cnode instance.

```
Cnode
+-- offload             <- HW offload flags (OF_IPV4_VAL, OF_TCP_VAL, ...)
+-- ether               <- Inner (normal-packet) configuration
|   +-- mtu             <- Inner MTU constraint
|   +-- dst / src       <- MAC addresses
|   +-- imix[]          <- IMIX packet size distribution
|   +-- n_imix          <- Number of IMIX entries
|   +-- type
|       +-- arp
|       +-- ipv4
|       |   +-- src / dst    <- IP range syntax (172.16.1.1+10 or arrays)
|       |   +-- proto        <- IP protocol number array
|       |   +-- icmp         <- ICMP ident[]
|       |   +-- tcp          <- TCP src/dst ports
|       |   +-- udp          <- UDP src/dst ports
|       +-- ...
+-- vxlan              <- Outer VXLAN encapsulation
|   +-- enable / ratio
|   +-- wire-mtu       <- Wire MTU cap (after VXLAN encap)
|   +-- ether / type / ipv4 / udp  <- Outer IP/UDP + VNI
+-- ext[]              <- Extension protocol config (SCTP, DNS, HTTP, NTP, ...)
+-- erroneous          <- Anomaly injection config
    +-- ratio          <- Mutation ratio (0-1023)
    +-- classes[...]   <- Per-protocol mutation classes
```

Extension protocols register their config parsers at compile time.  At config
time, `config_clone_cnode()` copies the base template and extension parsers
fill their slots into `cnode->ext[]`.  The framework does not need to know
about extension protocol internals -- it only calls the registered callbacks.


## 3. Design Principles

### 3.1 Entropy Calculation Is Not on the Fast Path

**Decision:** Entropy calculation runs on the main lcore, not on any TX/RX
worker lcore.

```
Worker lcores                    Main lcore
+---------------------+       +--------------------------+
|  worker_loop():      |       |  worker_main_loop():     |
|   while (1)          |       |   while (state != EXIT)  |
|     build mbufs      |       |     clock_nanosleep    |
|     tx_burst(mbufs)  |       |     if (timer_tsc >= T)  |
|     entropy_record()-+-->    |       compute_entropy()  |  <- qsort runs here
+---------------------+  ring  |       flow_entropy()     |
                              |       adapt_pps_rate()    |
                              |       stats_broadcast()   |
                              +--------------------------+
```

- `worker_loop()` only sends packets and writes samples -- no sorting.
- `worker_main_loop()` polls every 100ms and runs entropy calculation once per
  `timer_period` seconds (`conf/config.yaml` sets 1; `0` runs on every 100 ms
  loop iteration).
- Worker lcores post samples to the main lcore via a contention-free ring
  buffer (`entropy_sampler`).  The write is a single
  `__atomic_store_n(&write_idx, ...)` -- lock-free, wait-free.

**Actual cost of sorting:**  Each stats period processes at most 32,768 samples
(8 lcores x 4,096 ring size).  26 qsort calls (13 single-dim + 13 joint pairs)
total ~10-30ms.  For a 1-10s stats period this overhead does not affect data-plane
throughput.

**Why not a separate thread:**  The main lcore sleeps for 100ms per iteration
and wakes only for periodic statistics and adaptation work, so its idle time
exceeds 99%.  Adding another lcore or thread would increase architectural
complexity with zero benefit.

### 3.2 PRNG: xorshift64*

**Decision:** Use xorshift64* instead of `rand()`.

| Property | Value |
|------|-----|
| Algorithm | xorshift64* (Vigna, 2016) |
| Period | 2^64 - 1 |
| Statistical tests | Passes BigCrush (TestU01) and PractRand |
| Per-lcore state | `__thread`, independent, no sharing |
| Seed entropy source | `rdtsc() ^ pthread_self()`, >= 64-bit entropy across lcores |
| Zero-seed safety | Seed forced to 1 when 0 (xorshift degenerates to all-zeros) |

The standard library `rand()` has two fatal flaws for this application:

1. **Period too short** -- typical RAND_MAX=2^31-1.  At 10 Mpps the sequence
   repeats after ~200 seconds.
2. **Low-bit periodicity** -- Linear congruential generators exhibit short
   periods in low-order bits, causing bias in port number low bits.

**Why per-dimension PRNGs are not needed.**  Each dimension calls
`fast_rand_next()` for an independent output (`r = fast_rand_next(); src_ip = r
% range1; dst_ip = fast_rand_next() % range2`).  64-bit state + full diffusion
(xorshift + multiply) ensures adjacent outputs have no statistical correlation.
2^64-1 period means ~58,000 years of continuous operation at 10 Mpps.

If real mutual information anomalies are observed in the future (I(X;Y) > 0.1
on the heatmap), the most likely causes are, in order: (1) IP/port ranges too
small at the configuration level, (2) packet rate causing sample distribution
bias, (3) fixed packet intervals causing temporal correlation.  All are
unrelated to PRNG choice.

### 3.3 P Controller: Why P Instead of PID

**Decision:** Use a pure proportional (P) controller for entropy-adaptive rate
limiting.

```c
// src/worker.c
double cur = s->entropy_src_ip;
double error = bconf->entropy_target - cur;
double adjust = error * bconf->entropy_adapt_gain;  // P term only
bconf->pps_rate += (int64_t)adjust;
```

**Why P is sufficient:**

1. **Steady-state residual error is acceptable.**  Stress testing does not
   require precise setpoint tracking.  +/-5-10% entropy deviation does not
   affect stress-test validity.

2. **The derivative term (D) does more harm than good in noisy environments.**
   Entropy measurements have inherent noise (quantization error, Poisson
   arrival fluctuations, gateway nonlinear rate-limiting behavior).  A D term
   amplifies this noise, producing unnecessary PPS jitter.

3. **The integral term (I) introduces windup risk.**  If gateway drops cause
   PPS to be clamped to 0, the integral accumulates continuously.  On recovery,
   the I term releases a large adjustment causing PPS overshoot.  Safely adding
   I requires anti-windup logic; the complexity does not match the benefit.

4. **Adjustable gain already covers most use cases.**  Via CLI
   `--entropy-adapt-gain=<value>` or WebSocket runtime adjustment, users
   control responsiveness.  Higher gain = fast response (may overshoot), lower
   = smooth response (longer settling time).

**Stability condition:**  Linearizing around equilibrium (H ~ a * PPS), the
closed-loop transfer function is PPS[k+1] = (1 - a*K_p) * PPS[k] + K_p *
H_target.  Stability requires |1 - a*K_p| < 1, i.e. 0 < K_p < 2/a.

### 3.4 Interleaved Send & Spatial Locality

**Decision:** Apply a Fisher-Yates shuffle to mbufs before `tx_burst` to break
spatial locality and measure worst-case conntrack throughput.

Without interleave, a single batch of mbufs may contain multiple consecutive
packets belonging to the same flow.  The gateway's conntrack lookup benefits
from cache acceleration when hitting an already-cached flow, producing
throughput measurements higher than the actual worst case.

Interleave mode applies a Fisher-Yates shuffle to the mbufs array before
`tx_burst`, ensuring consecutively transmitted packets belong to different
flows -- every lookup triggers a cache miss -- measured worst-case throughput.

**Measurement metric: I(Delta t; flow_key).**  `flow_key` is a XOR-compressed
64-bit hash of the inner 5-tuple.  When interleave is off, same-flow packets
cluster temporally and a positive I(Delta t; flow_key) results.  When
interleave is on, flow and timing are decoupled and I -> 0.

**Interleave depth.**  `--interleave-depth=N` (1-100) controls what fraction
of each batch's packets participate in the shuffle.  Depth=100 is full shuffle
(original behavior).  Depth=50 shuffles only the first half.  This lets users
progress from "no locality breaking" to "full randomness," simulating different
NIC multi-queue distribution granularities.

- **CLI**: `--interleave=true` / `--interleave-depth=50`
- **Data-plane cost**: O(batch) pointer swaps, lock-free
- **Does not change packet characteristics**: shuffle only changes send order,
  not the 5-tuple distribution or entropy/MI values within each batch

### 3.5 Token Bucket Rate Limiter

**Decision:** Three independent token buckets per worker: PPS, BPS, and CPS.

Each worker has its own set of `struct token_bucket` (one each for PPS, BPS,
and CPS).  The handshake worker additionally enables the CPS bucket.  Refill
happens inline on the TX hot path using TSC for precise sub-second accounting.

**Token bucket model (CIR/CBS):**

- CIR (Committed Information Rate): tokens added per second.  A bucket with
  CIR=0 is disabled and returns UINT64_MAX from `token_bucket_available()`.
- CBS (Committed Burst Size): maximum tokens the bucket can hold.  Buckets
  start full (tokens=CBS).
- Refill: `refill = (cir * elapsed_tsc) / timer_hz`.  Overflow-safe: if
  cir*elapsed would exceed UINT64_MAX, refill is clamped to CBS.
- Consume: after each TX burst, `tokens -= n_sent`.  Never goes below 0.

**Runtime rate changes:** `token_bucket_set_rate()` changes CIR and resets
`last_tsc` to avoid an immediate token windfall from the elapsed time since the
last refill.

**Configuration:**

```yaml
injector:
  pps-rate: 10000       # 10 Kpps cap
  pps-burst: 512        # burst tolerance (0 = auto = batch*4)
  bps-rate: 12500000    # 100 Mbps cap
  bps-burst: 2097152    # burst tolerance (0 = auto = 65536)
# handshake mode only:
  hs-rate: 64           # SYNs per round
```

### 3.6 Traffic Models: Uniform, Poisson, Pareto

**Decision:** Provide a timing spectrum from deterministic to heavy-tailed.

| Model | Parameter | Implementation | Use Case |
|-------|-----------|---------------|----------|
| Uniform (0) | `batch_delay_us` + `batch_jitter_us` | `random_delay_jitter(base, jitter)`: `base + (r % (2*jitter+1)) - jitter` | Stress engine peak throughput |
| Poisson (1) | `batch_delay_us` as mean | `exp_random(mean)`: `-(double)mean * log(U)` where U ~ (0,1] | Simulate real network inter-arrival |
| Pareto ON-OFF (2) | `pareto_alpha` (shape, typ 1.0-2.0) | `pareto_random(scale, alpha)`: `scale / U^(1/alpha)` | Heavy-tail burst simulation |

**Exponential inverse-transform sampling** (`exp_random`):  Uses
`U = (r & 0x7FFFFFFF) / 2^31`, clamped to `[1e-10, 1]` to avoid `log(0)`.
Result rounded to nearest microsecond.

**Pareto distribution:**  alpha < 2 gives infinite variance (heavy tail).
alpha <= 1 gives infinite mean -- use with caution.  This model produces ON/OFF
bursts where ON periods are bursty and OFF periods are long-tail, simulating
real Internet traffic's self-similar characteristics.

**Jitter vs Poisson:**  Uniform jitter (`batch_jitter_us=50`) produces a
rectangular distribution -- all delays within [base-50, base+50] are equally
likely.  Poisson produces an exponential distribution peaking near 0 with a
long right tail.  For entropy measurement, Poisson's H(delta_tsc) is
significantly higher than uniform jitter for the same mean delay.

### 3.7 IMIX Packet Size Distribution

**Decision:** Support configurable IMIX (Internet Mix) to simulate real-world
packet size distributions.

IMIX sizes are specified as IP packet sizes (L3+L4+payload), not L2 frame
sizes.  The IMIX_PAYLOAD_LEN macro picks a random entry from the configured
list and computes `payload_len = imix_size - l3_len - l4_len`.

```yaml
bless:
  ether:
    imix: [64, 594, 1518]   # standard IMIX: 7:4:1 ratio via weight repetition
```

**Interaction with MTU:**  IMIX runs after MTU trimming.  If the random IMIX
value exceeds the effective MTU, the payload is clamped.  When VXLAN is
enabled, the effective MTU is `min(ether.mtu, vxlan.wire-mtu - VXLAN_overhead)`.

**Effect on entropy:**  Each IMIX size contributes -p*log_2(p) to
H(pkt_size).  A 3-size IMIX with 7:4:1 weights yields H ~ 1.38 bits.

### 3.8 TCP Handshake State Machine

**Decision:** A dedicated `handshake` mode that runs a full TCP
SYN->SYN-ACK->ACK state machine, measuring conntrack table pressure and
connection-level entropy.

**State machine:**
```
SYN_SENT  --(recv SYN-ACK)-->  ESTABLISHED  --(idle timeout)-->  TIMEOUT
                              ESTABLISHED  --(recv RST)------>  RST
```

**Data structures:**

- **Hash table:**  `struct handshake_ctx` contains a fixed-size hash table
  (`HS_HT_SIZE = 65536` slots) in hugepage memory.  Open addressing with linear
  probing.  Key is `{src_ip, dst_ip, src_port, dst_port}`.
- **Flow sampler:**  `struct flow_sampler` records connection lifecycle events
  (CREATED, ESTABLISHED, TIMEOUT, RST) per worker.  Used for flow-level
  entropy computation.

**Per-round cycle:**

1. **TX phase:**  Two sub-batches -- handshake and stateless, ratio controlled
   by `hs_mix_ratio` (0-1000, permille).  SYNs are constructed with random
   5-tuples and inserted into the hash table.  Stateless traffic uses the
   normal `bless_mbufs_tcp()` path.
2. **RX phase:**  Receive packets, match SYN-ACKs and RSTs against the hash
   table.  On SYN-ACK: promote entry to ESTABLISHED.  On RST: mark RST, remove
   from table.
3. **Cleanup:**  Each round scans a portion of the hash table for timed-out
   entries (default 10s idle timeout).  Linear scan amortized across rounds.

**Rate limiting:**  All three token buckets (PPS, BPS, CPS) apply.  The CPS
bucket limits new connections per second independently of packet rate.

**Measured metrics:**
- `hs_syn_sent`, `hs_synack_recv`, `hs_established`, `hs_rst_recv`,
  `hs_timed_out` -- event counters
- `hs_conn_current`, `hs_conn_max` -- connection table depth
- `hs_success_rate` -- established / syn_sent
- `hs_cps` -- connections per second
- `flow_entropy_5tuple`, `flow_entropy_lifetime`, `flow_entropy_event` --
  flow-level entropy

**Limitations:**
- Only TCP handshake is instrumented (no UDP or ICMP in handshake mode).
- Only supports RST termination, not FIN + TIME_WAIT.
- Single worker per lcore; no cross-lcore connection migration.

### 3.9 Latency Histogram

**Decision:** Embed TX timestamps in UDP payload to measure end-to-end
latency.

- **Timestamp embedding:**  In `bless_mbufs_udp()`, when
  `latency_hist_enable=true`, writes `rte_rdtsc()` into the first 8 bytes of
  the UDP payload (overwriting the payload preamble).
- **RX-side extraction:**  `worker_func_rx_only` and `worker_func_fwd` extract
  TSC from received packets, compute `delta_tsc -> us`, and write to per-worker
  `latency_hist`.
- **Histogram structure:**  14 logarithmic-scale buckets
  (0/1/2/5/10/20/50/100/200/500/1000/2000/5000/10000 us), per-worker
  cache-line-aligned.
- **Observer aggregation:**  `compute_entropy_stats()` iterates over all
  workers, `__atomic` reads and zeros each bucket, merges, computes
  p50/p95/p99/p999 via linear interpolation.
- **Reset strategy:**  After each stats window (~100ms), per-worker buckets are
  zeroed; the next window accumulates from scratch.

**Limitations:**
- Only embeds TSC in UDP packets; TCP/ICMP are not instrumented.
- Inner-layer UDP within VXLAN encapsulation is not extracted.
- Requires an external loop (DUT reflection or dual-end BLESS).
- `rte_rdtsc()` to us conversion depends on `rte_get_tsc_hz()`; CPU frequency
  changes may introduce error.

### 3.10 MI Diagonal Smoothing

**Decision:**  Apply EMA smoothing to the 12 diagonal entropy values to reduce
measurement noise at low PPS.

- **Parameter:**  `mi-smoothing-window` (uint32, default 1 = disabled).
- **Algorithm:**  EMA, alpha = 2/(window+1), H'(t) = a*H_raw(t) +
  (1-a)*H'(t-1).
- **Scope:**  12 diagonal dimensions: protocol, src_ip, dst_ip, src_port,
  dst_port, pkt_size, delta_tsc, tcp_flags, joint_5tuple, vxlan_encap,
  outer_src_ip, vni.
- **Frontend no change:**  Smoothing is done server-side; JSON already contains
  smoothed values.

```yaml
injector:
  mi-smoothing-window: 5
```

### 3.11 Layered MTU: VXLAN wire-mtu

**Decision:**  Add `vxlan.wire-mtu` as a layered constraint on top of
`ether.mtu`.  `ether.mtu` caps the inner packet (before VXLAN encapsulation).
`vxlan.wire-mtu` caps the total wire packet (after VXLAN encapsulation adds 50
bytes).  When VXLAN is off or `wire-mtu=0`, behavior is unchanged (pure
`ether.mtu` control).

```
ether.mtu: 1500          <- Inner packet total length cap
vxlan.wire-mtu: 1500     <- Wire packet total length cap (VXLAN enabled only)
```

When VXLAN is enabled and `wire-mtu > 0`, each protocol constructor computes:
```
inner_max = wire_mtu - VXLAN_overhead - l2_len
effective_mtu = min(ether.mtu, inner_max)
```

**Why layered, not unified:**  A single unified MTU would confound VXLAN ON/OFF
comparison experiments and cannot express "inner jumbo + outer standard MTU"
scenarios.  The layered approach keeps the inner layer consistent across VXLAN
ON/OFF.

---


## 4. Module Layout

### 4.1 Anomaly Injection Framework

After normal packet construction, an optional mutation stage applies destructive
modifications to packet headers:

| Category | Mutations |
|----------|-----------|
| mac | VLAN, multicast, IPv6 (3) |
| arp | src IP (1) |
| ipv4 | version, ihl, dscp, ecn, total_len, id, flags, frag_offset, ttl, proto, cksum, ipsec, frag (13) |
| icmp | type (1) |
| tcp | syn_flood, data_off, flags, window, cksum (5) |
| udp | len, cksum, vxlan (3) |
| sctp | cksum, port, vtag (3) |
| quic | version, cid_len, token_len (3) |
| ipv6 | version, traffic_class, flow_label, hop_limit (4) |
| dns | tc, nxdomain, qtype_invalid, label_overflow, id_zero (5) |
| http | malformed, method_invalid, host_overflow, crlf_missing (4) |
| ntp | stratum, li, version, timestamp_zero (4) |
| other | toa (1) |

Anomaly ratio is controlled by `erroneous.ratio` (0-1023), corresponding to
0%-100% at ~0.1% granularity.  The mutation function is selected randomly
per-packet in `worker_func_tx_only()`.

```yaml
bless:
  erroneous:
    ratio: 400          # ~39% mutation rate
    class:
      ipv4: [ version, ihl, cksum ]
      tcp:  [ syn_flood ]
```

---


### 4.2 Protocol Extension API

#### 1 Protocol Registration

New protocols register at compile time via `__attribute__((constructor))`:

```c
// src/proto_gre.c -- new protocol needs only this file
#include "bless_plugin.h"

static uint64_t gre_construct(struct rte_mbuf **mbufs,
                               unsigned int n, void *cfg) {
    Cnode *cnode = (Cnode *)cfg;
    // ... construct n GRE packets ...
    return total_bytes;
}

static const struct bless_pkt_type proto_gre = {
    .name       = "gre",
    .ether_type = RTE_ETHER_TYPE_IPV4,
    .ip_proto   = 47,
    .type_idx   = BLESS_AUTO_IDX,   // extension: auto-assign
    .construct  = gre_construct,
};

static void __attribute__((constructor)) reg_gre(void) {
    bless_register_pkt_type(&proto_gre);
    bless_set_type_weight("gre", 30);  // default weight
}
```

Built-in protocols (ARP/ICMP/TCP/UDP/SCTP) use explicit `type_idx` 0-4.
Extension protocols use `BLESS_AUTO_IDX` (255) for automatic assignment.

**Weights and distribution:**  New protocols automatically participate in
weighted distribution via `--weight gre=30` CLI or YAML `bless.ether.dist`.
`bless_set_dist()` iterates over all registered types with weight > 0.

#### 2 Config Parsing for Extensions

Extensions that need YAML configuration (port ranges, payload, etc.) register
a `struct bless_ext_cfg` parser:

```c
static struct bless_cfg_field sctp_fields[] = {
    BLESS_CFG_FIELD_PORT_RANGE("src", sctp_cfg, src_port, src_range),
    BLESS_CFG_FIELD_PORT_RANGE("dst", sctp_cfg, dst_port, dst_range),
    { NULL },
};

static const struct bless_ext_cfg sctp_ext_cfg = {
    .name = "sctp",
    .fields = sctp_fields,
    .port_range = sctp_port_range_cb,
    .cfg_size = sizeof(struct sctp_ext_cfg),
};

static void __attribute__((constructor)) reg_sctp(void) {
    bless_register_pkt_type(&proto_sctp);
    bless_register_cfg_parser(&sctp_ext_cfg);
}
```

YAML configuration:
```yaml
bless:
  ether:
    type:
      ipv4:
        sctp:
          src: 30000+50
          dst: 3868
```

#### 3 port_range Callback

Extension protocols automatically participate in port range aggregation for
entropy max computation via the `port_range` callback.  The framework calls
`bless_ext_aggregate_port_ranges()` which iterates all registered ext configs
and takes the max of each dimension.

---


### 4.3 VXLAN Encapsulation

VXLAN encapsulation adds 50 bytes of outer headers to each qualifying packet:

```
Outer Eth (14) + Outer IPv4 (20) + Outer UDP (8) + VNI (8) = 50 bytes
```

**Ratio control:**  `vxlan.ratio` (0-100) controls what percentage of packets
receive VXLAN encapsulation.  The decision is made per-packet via
`fast_rand() % 100 < ratio`.

**IP:VNI syntax:**  The outer source IP supports `IP:VNI` format (e.g.
`172.16.0.1:100+10` sets base VNI to 100 with a range of 10).  The destination
IP is pure IP only.

**IPv6 VXLAN:**  `BLESS_SIZEOF_VXLAN6 = 70 bytes` (Eth 14 + IPv6 40 + UDP 8 +
VNI 8).  Selected when the outer IP config contains an IPv6 address.

**MTU interaction:**  See 3.11 (Layered MTU).

---


### 4.4 Configuration System

Two-phase merge:

1. YAML config file (`conf/config.yaml` or specified on command line)
2. CLI parameter overrides (`--tcp=50 --udp=50`, etc.)

Priority: CLI > YAML > built-in defaults.

```
CLI
  +-- args_save_cli_overrides()
  +-- init_config()      <- reads YAML
       +-- parse_and_merge_config()  <- merges CLI overrides
            +-- init_runtime_state()  <- initializes runtime (distribution table, etc.)
```


### 4.5 File / Directory Structure

```
bless/
+-- Makefile                Top-level build
+-- README.md
+-- .github/workflows/ci.yml
+-- conf/
|   +-- config.yaml         Default config
|   +-- config-test.yaml    Test config (PCAP loopback)
|   +-- ...
+-- docs/
|   +-- overview.md         This document
|   +-- entropy-theory.md   Scientific foundations
|   +-- benchmarks.md       Methodology and results
|   +-- ...
+-- include/
|   +-- base.h              base global singleton
|   +-- bless.h             MBUF construction interface
|   +-- cnode.h             Config struct (core data structure)
|   +-- config.h            Config parsing
|   +-- bless_plugin.h      Protocol extension API
|   +-- entropy.h           Per-worker entropy sampler
|   +-- flow_entropy.h      Flow-level entropy (handshake mode)
|   +-- token_bucket.h      Rate limiter
|   +-- rate_psd.h          Frequency-domain PSD analysis
|   +-- mutation.h          Anomaly injection
|   +-- ...
+-- src/
|   +-- main.c              Entry point
|   +-- args.c              CLI argument handling
|   +-- config.c            YAML config parsing
|   +-- bless.c             Packet construction engine
|   +-- dist.c              Protocol weight distribution (testable without DPDK)
|   +-- parse.c             IP/port range parsing (framework-independent)
|   +-- worker.c            Worker main loop (all modes)
|   +-- server.c            WebSocket control plane
|   +-- entropy_compute.c   Entropy calculation
|   +-- flow_entropy.c      Flow entropy analysis
|   +-- token_bucket.c      Rate limiter init
|   +-- rate_psd.c          FFT + spectral analysis
|   +-- proto_arp/icmp/tcp/udp/sctp/dns/http/ntp/quic/ipv6.c
|   +-- mutation_dns/http/ntp.c  Per-protocol mutation functions
+-- test/unit/
|   +-- test_dist.c         Distribution unit tests
|   +-- test_core.c         Parse/IP range unit tests
|   +-- test_ipv6.c         IPv6 range unit tests
|   +-- test_device.c       Device classification tests
+-- third_party/            civetweb, cJSON, libyaml
+-- tools/                  CI, HTML generators, benchmark scripts
```

---



## Further Reading

### Concepts

| Document | Description |
|----------|-------------|
| [`concepts/entropy-theory.md`](concepts/entropy-theory.md) | Scientific foundations: entropy, mutual information, three-axis model, P controller |
| [`concepts/min-entropy.md`](concepts/min-entropy.md) | Min-entropy measurement methodology |
| [`concepts/entropy-interpretation.md`](concepts/entropy-interpretation.md) | Configuration-aware baselines, gaps and diagnostic model |
| [`concepts/mutual-info.md`](concepts/mutual-info.md) | Mutual information pairs, heatmap interpretation |
| [`concepts/rate-psd.md`](concepts/rate-psd.md) | Frequency-domain PSD traffic analysis |

### User Guide

| Document | Description |
|----------|-------------|
| [`guides/config.md`](guides/config.md) | YAML and CLI configuration reference |
| [`guides/runtime.md`](guides/runtime.md) | Runtime parameter changes via WebSocket |
| [`guides/docker.md`](guides/docker.md) | Container deployment |
| [`guides/profiling.md`](guides/profiling.md) | Flame graphs and perf analysis |
| [`guides/observability.md`](guides/observability.md) | Prometheus and Grafana integration |
| [`guides/dashboard.md`](guides/dashboard.md) | Web dashboard pages |
| [`guides/security.md`](guides/security.md) | Security policy and reporting |
| [`guides/parsing.md`](guides/parsing.md) | IP range, port range, IPv6 range syntax |

### Developer Guide

| Document | Description |
|----------|-------------|
| [`reference/BUILDING.md`](reference/BUILDING.md) | Build from source, system dependencies |
| [`reference/api.md`](reference/api.md) | HTTP/WebSocket API and server configuration |
| [`reference/concurrency.md`](reference/concurrency.md) | Memory model for runtime config updates |
| [`design/pacing-backend.md`](design/pacing-backend.md) | Software, hardware-rate, and scheduled-send pacing design |
| [`guides/error-handling.md`](guides/error-handling.md) | Error severity hierarchy and conventions |
| [`reference/fuzzing.md`](reference/fuzzing.md) | Fuzzing setup and corpus management |
| [`contributing/documentation.md`](contributing/documentation.md) | How to contribute to Bless documentation |

### Reference

| Document | Description |
|----------|-------------|
| [`reference/project-assessment-2026-07-28.md`](reference/project-assessment-2026-07-28.md) | Revision-specific assessment of architecture, correctness, security, testing, deployment, and governance |
| [`reference/remediation-roadmap.md`](reference/remediation-roadmap.md) | Three-stage remediation program with the detailed Phase 1 backlog |
| [`reference/phase1-follow-up-plan.md`](reference/phase1-follow-up-plan.md) | Ordered checklist and acceptance criteria for the remaining Phase 1 work |
| [`reference/protocols/dns.md`](reference/protocols/dns.md) | DNS, NTP, HTTP protocol constructors |
| [`reference/protocols/ipv6.md`](reference/protocols/ipv6.md) | IPv6 packet construction |
| [`reference/protocols/quic.md`](reference/protocols/quic.md) | QUIC protocol support |
| [`reference/priority.md`](reference/priority.md) | Development priorities and planning |
| [`reference/config-architecture.md`](reference/config-architecture.md) | Configuration ownership, module boundaries, and remaining split |
