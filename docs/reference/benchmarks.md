# Performance Benchmarks -- bless

## 1. Methodology

### Metric definitions

| Term | Definition | Caveats |
|------|-----------|---------|
| **IP packet size (B)** | User-configured `imix[N]` value -- the L3 payload target size | Reported in all tables; includes IP header + L4 header + payload |
| **L2 packet size (B)** | IP packet size + 14 (Ethernet header) | Excludes FCS (4 B) added by NIC |
| **L2 Gbps** | `L2 packet size x MPPS x 8` | Ethernet throughput at L2; excludes FCS, preamble, SFD, IFG |
| **Wire occupancy (B)** | L2 packet + 4 (FCS) + 8 (preamble 7 B + SFD 1 B) + 12 (IFG) | Physical link utilisation including all overhead bytes |
| **Wire-rate Gbps** | `Wire occupancy x MPPS x 8` | Effective line rate -- what the NIC actually transmits onto the wire |
| **IMIX** | Weighted mix of frame sizes randomly selected per packet | Default 7:4:1 ratio (64:594:1500) |
| **P50 / P95** | Median / 95th percentile of samples | Within-run unless "between-run" noted |
| **sample-interval** | Per-worker: sample one 5-tuple every N packets for entropy ring-buffer | `0` = sampler disabled entirely; `1` = sample every packet |

### Sampling protocol

```
Within-run:   5-10 consecutive 1-second samples after 5 s warm-up
Between-run:  3-5 independent process restarts with full DPDK re-init
Reporting:    P50 (median) of all samples within that series
Variance:     min, max, and spread noted where >=+/-3%
```

**Important limitation:** Within-run samples from one process invocation are NOT equivalent to independent experiments.  KVM guest scheduling, host CPU contention, and DPDK init state can produce between-run variance exceeding within-run noise.  Where between-run data is available, it is reported explicitly; otherwise conclusions are limited to the observed run.

For feature-overhead experiments where deltas as small as +/-5% are of
interest, at least 5 independent process restarts per scenario are
required before drawing conclusions.  Within-run samples from a single
invocation are sufficient for gross effects (>10%) and for identifying
trends, but they understate real-world variance.

---

## 2. Test Environment

### Software

| Property | Value |
|----------|-------|
| **OS** | Debian, Linux 6.1.0-37-amd64, x86_64 |
| **Virtualisation** | KVM/QEMU (i440FX + PIIX, host-passthrough CPU) |
| **CPU** | 40 vCPUs -- 2 sockets x 20 cores, Intel Xeon Gold 6148 @ 2.40 GHz |
| **NUMA** | 2 nodes: node0=cores 0-19, node1=cores 20-39 |
| **L1/L2/L3** | 1.3 MiB / 160 MiB / 32 MiB (2 instances, 1 per socket) |
| **Memory** | 40 GB, 1G hugepages: 11 total / 10 free (10 GB) |
| **DPDK** | 23.11.3 (ABI 24.0), installed at `/opt/dpdk`, static linkage |

### Hardware

| Property | Value |
|----------|-------|
| **NIC** | Mellanox ConnectX-6 PF (MT28908), PCI `0000:00:04.0` |
| **Interface** | `ens4np0`, MAC `10:70:fd:e5:d5:20` |
| **Link** | **NO-CARRIER** -- PHY has no link partner |
| **Driver** | mlx5_core (kernel), mlx5 PMD (DPDK, no vfio-pci needed) |
| **E-switch** | `mode legacy`, `encap-mode basic` |
| **TX queues** | Up to 64 (full PF resources) |

### PMD matrix

| PMD | I/O path | Measures | When used |
|-----|---------|----------|-----------|
| `net_null` | No I/O (drops TX) | Pure software construction + DPDK TX API overhead | Sections 3.1-3.2 |
| ConnectX-6 PF | PCIe DMA to real NIC | Current mlx5 TX-path reference | Sections 3.3-3.4, 3.6 |
| DPDK testpmd | Static mbuf recycling | Static-mbuf testpmd reference | Section 3.5 |

---

## 3. Completed Results

### 3.1 Batch Size Sweep (net_null)

**Hypothesis:** Batch size affects amortisation of `rte_eth_tx_burst()` overhead.
Larger batches reduce per-burst overhead but may increase mbuf pool contention.

**Config:** `conf/config-bench.yaml` -- single core, net_null PMD, UDP+TCP+SCTP mix (1:3:1),
`sample-interval=0`, `copy-payload=false`, `hw-offload=[]`, `batch-delay-us=0`.

| Batch | MPPS (P50) | Note |
|-------|-----------|------|
| 1 | 2.20 | Worst case -- one mbuf alloc per tx_burst |
| 8 | 2.84 | Already at 97% of peak |
| 16 | 2.93 | Near peak |
| **128** | **2.94** | Sweet spot -- selected as default for subsequent tests |
| 256 | 2.75 | Mild regression |
| 512 | 2.52 | Further regression -- mbuf pool contention |

**Commit:** `59f933b`, config SHA: `conf/config-bench.yaml` (unchanged since).

**Interpretation:** Batch=128 is the optimal trade-off.  Used as fixed parameter for all subsequent tests.

---

### 3.2 Frame-Size Sweep (net_null)

**Hypothesis:** Larger frame sizes reduce PPS (more work per packet) but increase Gbps
(amortised overhead per byte).  The relationship reveals whether construction is
CPU-bound by per-packet overhead or memory-bandwidth-bound by payload size.

**Config:** `conf/config-bench.yaml` with per-run `imix` override.
Batch=128, single core, net_null PMD, `sample-interval=0`, `copy-payload=false`.

**Frame size semantics:**

The `imix: [N]` value controls the **IP packet size** (L3 payload target).
The corresponding L2 packet adds 14 B for the Ethernet header (FCS not included).
`bless_tx_gbps` is computed as `L2 packet size x PPS x 8`.

```
IP packet size (imix[N])  + 14 (Eth hdr)           = L2 packet size
L2 packet                 + 4 (FCS) + 8 (preamble 7B + SFD 1B) + 12 (IFG)
                                                     = Wire occupancy
Gbps        = L2 packet size x MPPS x 8
Wire Gbps   = Wire occupancy x MPPS x 8
```

Ethernet wire-level overhead: 7 B preamble + 1 B SFD = 8 B preamble/SFD field.
FCS (4 B CRC) is appended by the NIC and not visible to DPDK or the driver.
IFG (12 B inter-frame gap) carries no data.

| IP pkt (B) | L2 pkt (B) | Wire (B) | MPPS (P50) | L2 Gbps | Wire Gbps |
|-----------|-----------|----------|-----------|---------|----------|
| 64 | 78 | 102 | 2.451 | 1.530 | 2.000 |
| 128 | 142 | 166 | 2.436 | 2.767 | 3.235 |
| 256 | 270 | 294 | 2.190 | 4.730 | 5.151 |
| 512 | 526 | 550 | 1.823 | 7.673 | 8.021 |
| 1024 | 1038 | 1062 | 1.384 | 11.495 | 11.757 |
| **1500** | **1514** | **1538** | **1.108** | **13.419** | **13.635** |
| IMIX 7:4:1 | mixed | *398.3* | 1.650 | 9.662 | 5.259 |

**IMIX overhead** compared to a service-time-weighted estimate from
homogeneous-size runs.  The correct ideal for a fixed-ratio workload is
the **harmonic mean** (not arithmetic):

```
Harmonic mean = 12 / (7/2.451 + 4/1.763 + 1/1.108)
              = 12 / (2.856 + 2.269 + 0.903)
              = 12 / 6.028
              = 1.991 MPPS
```

Note: 1.763 is the MPPS for 594 B (7:4:1 IMIX middle size), measured
separately and consistent with the size curve.

Overhead = `1 - 1.650 / 1.991 ~ 17.1%` (not 22%, which was the result of
incorrectly using an arithmetic weighted average of PPS instead of the
harmonic mean).

**Commit:** `59f933b`.

**Interpretation:**

- **PPS declines as the configured packet length increases** (2.45 -> 1.11 MPPS),
  indicating a size-dependent cost somewhere in packet construction or
  mbuf handling.  Profiling is required to attribute the cost (see §3.2.1).
- **L2 Gbps rises** from 1.53 to 13.42 Gbps.  The software construction path is
  capable of producing an equivalent byte rate above 10 Gbps before real NIC
  I/O is introduced.  This does **not** predict physical-NIC throughput -- net_null
  has no PCIe, DMA, NIC queue, descriptor reclaim, or wire transmission.
- **IMIX mix has ~17% overhead** vs the harmonic-mean ideal.  This residual cost
  may come from random size selection, branch unpredictability, heterogeneous
  packet-processing paths, or cache effects.  Decomposing the overhead
  (PRNG cost, branch predictor, burst heterogeneity, size cost) requires
  controlled experiments with fixed 7:4:1 periodic sequences vs random
  selection vs homogeneous bursts -- currently not measured.
- **Pending:** perf profiling of 64B, 512B, 1500B runs to attribute the
  size-dependent cost.

#### 3.2.1 Perf Profiling Results (net_null, 20M pkts/run)

**Method:** `perf stat` with the same config as §3.2, reduced to 20M
packets to keep run time manageable under perf overhead (~30%).

**Config:** `conf/config-bench.yaml`, per-run `imix` override, batch=128,
single core, net_null PMD, `sample-interval=0`, `copy-payload=false`.

| Metric | IMIX=64 | IMIX=512 | IMIX=1500 | 64->1500 Delta  |
|--------|---------|----------|-----------|----------|
| **cycles/pkt** | 1,389 | 1,840 | 2,987 | +115% |
| **insts/pkt** | 1,109 | 2,015 | 4,099 | +270% |
| **IPC** | 0.80 | 1.10 | 1.37 | -- |
| branch-miss % | 2.24 | 1.23 | 0.56 | -- |
| L1-dcache-miss % | 4.46 | 3.92 | 3.59 | -- |
| elapsed (s) | 11.27 | 13.07 | 20.43 | -- |

**Commit:** `9c5c656` (skill `bless-performance-experiments`).

**Interpretation:**

- **instructions/pkt scales 3.7x from 64->1500**, far exceeding the
  packet-size ratio (23.4x).  The baseline construction cost (L4 headers,
  distribution dispatch, mbuf metadata) dominates at 64 B.
- **IPC improves from 0.80->1.37**: the larger-packet code path has fewer
  front-end stalls.  **branch-miss rate drops from 2.24%->0.56%**,
  consistent with more linear code in the larger payload handling path.
- **Limitation:** perf stat introduces ~30% measurement overhead (observed
  1.77 MPPS at 64 B vs bare-benchmark 2.45).  Relative trends are valid;
  absolute cycles/pkt numbers are upper bounds.
- **Next:** `perf record` with flame-graph generation would attribute the
  additional 812 insts/pkt (1,109 -> 2,015 -> 4,099) to specific functions.
  A zero-point decomposition (testpmd txonly cycles) is also needed --
  see preliminary results below.

#### 3.2.2 testpmd Zero-Point Comparison

**Method:** `perf stat` on DPDK's built-in testpmd in txonly mode
(64 B static mbufs, single core, ConnectX-6 PF), compared to bless at
IMIX=64.  testpmd's txonly path performs zero packet construction,
providing a lower bound on the DPDK TX pipeline cost per packet.

**testpmd config:** `--forward-mode=txonly --txpkts=64 --nb-cores=1
--txq=1 --rxq=1 --total-num-mbufs=200000`

| Metric | testpmd (HW, 64B) | bless (net_null, 64B) | bless extra |
|--------|-------------------|-----------------------|-------------|
| **cycles/pkt** | 897 | 1,389 | +492 (+55%) |
| **insts/pkt** | 297 | 1,109 | +812 (+273%) |
| **IPC** | 0.33 | 0.80 | -- |
| TX packets | 1,065,152 | 20,000,000 | -- |
| TX drops | 37,120 (3.4%) | 0 (net_null) | -- |

**Commit:** `9c5c656` (same experiment series).

**Structural limitations of this comparison:**

- testpmd ran on real hardware (mlx5 PMD, PCIe DMA) while bless ran on
  net_null (no I/O).  The IPC difference (0.33 vs 0.80) reflects this --
  testpmd spends most cycles waiting for hardware (memory-mapped I/O
  stalls), while bless is purely CPU-bound.  Direct cycle subtraction
  between the two workloads is not meaningful.
- **A lower-bound estimate of Bless's software construction cost** is
  the instruction-count difference: **~812 instructions per 64 B packet**
  beyond what a zero-construction DPDK TX path requires.  Attributing
  these instructions to specific sub-components (mbuf alloc, header fill,
  software checksum, distribution dispatch) requires further
  single-variable isolation experiments.

#### 3.2.3 Entropy Sampler Overhead (net_null)

> **Superseded by §6.10.**  The data below used within-run PPS only and
> did not use the matched perf/counter window.  The new methodology
> (§6.3) shows the sampler adds +398 inst/pkt at interval=1 (HW PF),
> not "below measurement threshold."

**Hypothesis:** The entropy sampler's per-packet `entropy_sampler_record()`
call adds measurable overhead to the fast path.

**Config:** `conf/config-bench.yaml`, batch=128, IMIX=[64], single core,
net_null PMD, `copy-payload=false`.  Variable: `sample-interval` swept
0, 1, 8, 64, 1024.  `0` = sampler disabled entirely; `1` = sample every
packet; higher values sample every Nth packet.  50M packets/run, 5
within-run samples.

| `sample-interval` | MPPS (P50) | Notes |
|-------------------|-----------|-------|
| 0 (sampler off) | **2.904** | Baseline |
| 1 (every packet) | **2.865** | -1.3% |
| 8 | **2.989** | +2.9% |
| 64 | **2.871** | -1.1% |
| 1024 | **3.015** | +3.8% |

**Commit:** `9c5c656` (skill `bless-performance-experiments`).

**Interpretation:** All values fall within a +/-4% band around baseline
(2.87-3.02 MPPS), which is consistent with KVM noise in this
environment.  **Under this superseded within-run methodology, the
sampler cost appeared to be below the measurement threshold.**
No systematic overhead trend is visible across the 1000x sampling-rate
range.  This conclusion was overturned by the matched-window methodology
in §6.10.

**Caveats:**

- `bless_process_cpu_cores` reports aggregate process CPU seconds per
  wall-clock second.  `bless_enabled_lcore_utilization_ratio` divides that
  value by `bless_enabled_lcores`; it describes consumption of the configured
  lcore capacity, not utilization of an individual PMD worker.  The deprecated
  `bless_cpu_busy_pct` is the ratio expressed as a capped percentage.
  None of these process-level metrics separates worker-core overhead (the
  `entropy_sampler_record()` push) from main-core overhead (Shannon entropy
  batch computation).  Use per-thread `/proc` or perf measurements for that
  decomposition.
- The 2026-08-15 revalidation found both sampler settings pinned at about 2.0
  busy-polling cores.  This equal occupancy does not establish zero sampler
  cost.  The between-run throughput P50 changed by -0.12% when paced and
  -0.27% when unlimited, below the observed between-run noise; no throughput
  cost was resolved by this experiment.  In unlimited mode,
  `sample-interval=10` also produced sampler overwrites (~1,000 per 20 s
  window), so the entropy window was incomplete. The metric uses the sum of
  per-thread `/proc/self/task/*/stat` CPU ticks over a window of at least one
  second so busy-polling worker time is included on nohz_full/isolcpus hosts.
- `bless_sampler_dropped` at `sample-interval=0` reported ~74K drops
  (theoretically should be 0 -- the sampler is disabled).  **Fixed**
  (commit `bb41c98`): two chained 0->100 fallback conversions in
  `entropy_sampler_init()` and the worker init path were overriding
  the explicit 0-disable.  After fix, interval=0 correctly reports
  drops=0.

---

### 3.3 Feature Overhead (ConnectX-6 PF)

**Hypothesis:** Each composable feature (VXLAN encap, erroneous mutation, entropy sampling)
has a measurable PPS cost.  Combined features may show sub-additive or super-additive
behaviour depending on shared code paths.

**Config:** Single core, batch=128, IMIX=[64], ConnectX-6 PF (`0000:00:04.0`), NO-CARRIER.
Per-run `conf/config-hw-*.yaml`.  50M packets each, 5 within-run samples, P50.

| Scenario | MPPS (P50) | Delta | Overhead |
|----------|-----------|-------|----------|
| Baseline (UDP only, no VXLAN, no erroneous) | 3.098 | -- | -- |
| +VXLAN (100% encap) | 2.021 | -1.077 | **-34.8%** |
| +Erroneous (~9.8% mutation) | 2.839 | -0.259 | -8.4% |
| +High-entropy mixed workload | 2.341 | -0.757 | -24.4% |

**Commit:** `59f933b`.  Script: `tools/bench_h5_features.py`.

**Interpretation:**

- **VXLAN incurs the largest cost** (-34.8%).  This includes additional
  per-packet construction work (outer Eth/IP/UDP/VXLAN header writes,
  prepend/headroom operations, outer checksum) plus the larger DMA
  transfer (64->114 B at 100% encap).  The benchmark does not yet
  separate CPU construction cost from DMA/PMD transfer cost.
- **Erroneous overhead is moderate** (-8.4%): ~10% branch probability +
  mutation function call overhead, within single-core tolerance.
- **+High-entropy mixed workload** (-24.4%) is not a single-feature
  measurement.  It simultaneously changes protocol distribution
  (ARP+ICMP+TCP+UDP mix), enables 30% VXLAN, and adds ~4.9% erroneous
  mutation.  The -24.4% delta combines all three effects and cannot be
  attributed to any one feature.  This row should be read as a realistic
  composite workload, not as an "entropy sampler overhead" measurement.
- **Open question -- what does the entropy sampler itself cost on the
  fast path?**  This question was subsequently measured in §6.10
  (T7-T8) under the matched-window methodology: sampler int=1 adds
  +398 inst/pkt, int=64 adds +130 inst/pkt.
- **Limitation:** Within-run samples only -- all scenarios sampled from
  one process invocation each.  Between-run variance not measured.
  Feature deltas near +/-5% should be treated as inconclusive until
  confirmed by at least 5 independent process restarts per scenario.
  The baseline value itself varies ~5% between runs (3.07 in H2 vs
  3.098 here), which is consistent with KVM scheduling noise.

---

### 3.4 Multi-Queue TX Performance (ConnectX-6 PF)

**Hypothesis:** Adding TX queues (one per worker) eliminates queue-level contention
and improves aggregate throughput.

**Config:** `conf/config.yaml.test`, TCP+UDP mixed flow, batch=512,
50M-100M packets per run, ConnectX-6 PF NO-CARRIER.

| Queues | Workers | Aggregate MPPS | pkt/s/core | vs q=1 |
|--------|---------|---------------|------------|--------|
| q=1 | 1 | **3.14** | 3,135,964 | 1.00x |
| q=4 | 4 | **5.38** | 1,345,676 | 1.71x |
| q=8 | 8 | **5.40** | 675,527 | 1.72x |

**Commit:** `15ce558`.  Script: `tools/bench_mq_tx.py`.

**Interpretation:**

- **q=4->8 zero gain** (5.38 -> 5.40 MPPS, <0.4% difference).  Multi-queue
  provides no benefit beyond 4 queues.
- **Per-core efficiency collapses** from 3.14M to 0.68M pps/core -- each
  additional worker contends for the same upstream resources.
- **Increasing queue count beyond four did not improve aggregate
  throughput in this environment.**  This observation rules out a
  simple one-queue serialisation explanation, but does not uniquely
  identify the bottleneck.  Candidate limits include packet
  construction cost, shared memory-pool resources, vCPU scheduling,
  mlx5 submission/reclaim behaviour, NUMA placement, and the observed
  PCIe slot power constraint (27W, documented in the Risk
  Matrix).  A controlled experiment with a single-variable sweep (e.g.
  pre-computed packet templates to eliminate construction cost, or
  hardware offload toggles) would be needed to isolate the dominant
  factor.

---

### 3.5 testpmd TX-path reference under NO-CARRIER

**Goal:** Run DPDK's testpmd txonly mode on the same passthrough NIC
(ConnectX-6 PF, NO-CARRIER) to provide a static-mbuf, zero-construction
reference point.  This is not a bare-metal ceiling -- the environment is
a KVM guest with PCI passthrough, and the port has no link partner.
Results reflect mlx5 PMD TX descriptor-path throughput under current
passthrough conditions, not a validated wire-rate limit.
bless with equivalent single-flow UDP config should be some fraction
of this reference.

**testpmd config:**

```bash
dpdk-testpmd -l 0-1 -n 2 --allow=0000:00:04.0 -- \
  --forward-mode=txonly --txpkts=64 --stats-period=1 \
  --nb-cores=1 --txq=1 --rxq=1 --portmask=0x1
```

**bless config:** `conf/config-hw-pktgen-equiv.yaml` (single-flow UDP, 64B,
fixed MAC/IP/Port, no VXLAN, no erroneous, batch=128, single core).

| Metric | testpmd txonly | bless pktgen-equiv | Ratio |
|--------|---------------|---------------------|-------|
| **Steady-state MPPS** | **46.35** | 2.92 (P50, n=8) | **6.3%** |
| TX drops | 62M / 1.89B (3.3%) | -- | -- |
| Total sent | 1.83B | 50M | -- |

testpmd samples (stable region, 16 points):
```
46.35  46.34  46.34  46.19  46.35  46.35  46.35  46.32
46.38  46.30  46.26  46.36  46.32  46.20  46.19  44.42
```
P50: **46.34 MPPS**, sigma < 0.5 MPPS.

bless samples (8 points):
```
2.97  3.01  3.02  2.55  2.91  2.93  2.89  2.80
```
P50: **2.92 MPPS**.

**Commit:** `84f5edd`.  Date: 2026-07-18.

**Interpretation:**

- The 16x gap is **qualitatively consistent** with the additional work
  performed by bless: mbuf pool dequeue, Eth/IP/UDP header construction
  (MAC + IP + port), software IP checksum (`hw-offload: []`),
  distribution dispatch, and doorbell write.  testpmd txonly performs
  none of these -- it recycles pre-allocated static mbufs through the
  TX descriptor ring with zero per-packet construction cost.
  **A Reference Ladder decomposing this gap was subsequently completed
  in §6** (T1-T11, matched-window methodology).
- **bless 2.92 MPPS consistent with H2 baseline** (3.07, <5% difference).
  Validates H2 data reproducibility.
- **testpmd reported a 3.3% TX shortfall/drop count at this rate.**
  The exact cause was not isolated and may involve descriptor
  availability, PMD reclaim behaviour, link-down handling, or other
  device-path limits.  Attribution to a specific mechanism requires
  rte_eth_stats, mlx5 xstats, descriptor completion/reclaim counters,
  and ideally a linked peer for end-to-end accounting.
- **pktgen not used** because `/opt/pktgen/bin/pktgen` links against DPDK
  24.x ABI (incompatible with system's 23.11.3) and its interactive TUI
  requires a real PTY (exits 255 without one).  testpmd txonly is a cleaner
  reference -- no pktgen-specific construction overhead.

**Conclusion:** bless achieves 6.3% of the testpmd static-mbuf TX rate
under these passthrough KVM + NO-CARRIER conditions.  This ratio should
not be interpreted as a general efficiency figure -- it is specific to the
current ConnectX-6 PF passthrough environment with no link partner, a
known PCIe power constraint (27W), and a single testpmd configuration.

---

## 4. Invalidated or Inconclusive Experiments

### 4.1 Multi-Core Scaling (net_null)

**Attempted:** Measure per-lcore lock-free design scaling from 1 to 32 cores.

**Result:** Aggregate MPPS stayed flat at ~2.6 regardless of worker count:

| Cores | Aggregate MPPS | Per-core MPPS | Efficiency |
|-------|---------------|---------------|------------|
| 1 | 2.611 | 2.611 | 100% |
| 2 | 2.654 | 1.327 | 51% |
| 4 | 2.664 | 0.666 | 26% |
| 8 | 2.629 | 0.329 | 13% |
| 16 | 2.482 | 0.155 | 6% |
| 32 | 2.645 | 0.083 | 3% |

**Why this experiment did not measure multi-core scaling:** The benchmark
configuration exposed only one TX queue (`rte_eth_dev_configure(port,
0, 1, ...)`), and all workers were mapped to that queue.  Therefore the
experiment cannot measure per-queue or per-core scaling -- it measures
many workers contending for a single queue resource.  Measuring
multi-core throughput would require either multiple TX queues per port
(with appropriate per-worker queue assignment) or multiple net_null
ports with per-worker port assignment.

**Disposition:** Invalidated -- the benchmark configuration did not provide
the per-worker queue isolation needed to measure scaling.  The
single-queue constraint is a configuration choice, not a fundamental
limitation of net_null or bless.  Multi-worker throughput measurement
is deferred to the hardware phase where multiple TX queues per port
are available.

---

## 5. Pending Experiments

*Detailed methodology for sampler single-variable testing, independent
process restarts, and perf profiling is maintained in the
`bless-performance-experiments` skill.  The sections below summarise
the current status and blocking conditions.*

### 5.1 Multi-Core Scaling (ConnectX-6 PF)

Blocked on PCIe slot power remediation (27W constraint detected).
H4 preliminary results showed aggregate throughput plateau at ~5.3 MPPS regardless
of core count -- consistent with multi-queue findings (§3.4).

### 5.2 pktgen Comparison

Blocked on DPDK ABI mismatch (pktgen binary requires DPDK 24.x; system
has 23.11.3).  testpmd comparison (§3.5) serves as a substitute
reference point for the current passthrough environment.

### 5.3 TRex Comparison

Not yet attempted.  Would require a separate TRex installation and
equivalent traffic profile configuration.

---

## Risk Matrix

| Risk | Probability | Impact | Mitigation |
|------|-----------|--------|------------|
| mlx5 PMD fails to init PF | Low | Blocked | Switch to VF (`00:05.0`) as fallback |
| PF crash (kernel panic) | Very Low | Host reboot | Small-scale first; dmesg monitor |
| VFs disrupted | Low | ens5-10 offline | VFs are independent; mlx5_core mgmt intact |
| NO-CARRIER changes mid-run | Very Low | False safety | Pre-flight check; no cable present |
| PPS counter inaccurate | Low | Wrong numbers | Cross-check `dpdk_opackets` vs `bless_tx_mpps` |
| PCIe slot power insufficient | **Confirmed** | TX throttling | 27W constraint detected; use different slot or reduce TX queues |
| Multi-core segfault (0x17ff78330) | **Fixed** [OK] | -- | commit `2b13b98`: 200ms PMD drain delay before `rte_eth_dev_stop()` |

---

## 6. Reference Ladder -- Cost Decomposition

### 6.1 Rationale

**Positioning.** The Reference Ladder is Bless's long-lived performance
baseline: a persistent tier set plus its attribution structure.  Feature
isolation is not a separate methodology outside the ladder -- it is the
single-variable diff performed by *adding a clean tier*.  When a
performance change cannot be explained by the existing tiers, the action
is to add a new clean tier that isolates the variable, not to switch
method.  A new tier is warranted when (a) a new feature enters the hot
path with no corresponding tier, (b) a measured tier deviates from the
additivity model by more than the measurement spread, or (c) a composite
workload's delta cannot be attributed to a single feature.  Section 6.14
lists the currently open candidates.

Three rules bind every tier:

1. **Clean vs cross-config is a hard boundary.**  Only tiers that share
   the same inner config produce attributable deltas.  A tier that also
   changes the protocol distribution or IP range is an exploratory
   composite, not an isolation -- Section 3.3's "+High-entropy" row
   changed protocol mix, VXLAN ratio, and mutation rate simultaneously,
   so its -24.4% cannot be attributed to any single feature.
2. **Matched-window methodology is a prerequisite, not an option.**
   Feature costs near +-5% are smaller than the systematic error of a
   mismatched window.  Section 6.2 invalidated all prior data for this
   reason: HW checksum offload appeared to cost -5 inst/pkt under the
   old methodology when the true cost is -278 inst/pkt.  Every tier must
   use the matched counter/perf window (Section 6.3).
3. **"Benefit" has three distinct, non-interchangeable definitions.**
   `MPPS` (throughput), `inst/pkt` (per-packet software cost), and
   `cores-saved` (resource reduction) are not equivalent.  Under
   busy-poll a worker burns 100% of a core regardless of instruction
   count, so an inst/pkt reduction need not save a core; under
   NO-CARRIER, MPPS is a construction-path metric, not end-to-end
   throughput.  State which definition a tier targets *before*
   measuring, and never compare deltas across definitions.

A single `testpmd 46 MPPS vs bless 3 MPPS` comparison cannot answer
*why* bless is slower.  The Reference Ladder decomposes the gap into
discrete, measurable layers.  Tiers are grouped into **clean A/B
comparisons** (shared inner config, single-variable diff) and
**cross-config exploratory tiers** (different inner configs, deltas
include configuration changes):

```
                    testpmd txonly  ->  PMD TX reference (minimal
                                         packet-construction overhead)
                           ↓
                bless bench-template ->  mbuf alloc + memcpy template
                                         + TX burst
                          ╱ ╲
                         ╱   ╲
    Clean A (fixed-UDP inner):       Clean B (3-protocol inner):
    ─────────────────────────        ──────────────────────────
    T3  fixed-UDP (baseline)         T6  distrib (baseline)
    T4  + sw cksum explicit          T7  + sampler int=1
    T5  + hw cksum offload           T8  + sampler int=64
    T9  + mutation threshold=100
                                     Cross-config (mixed inner):
    ───────────────────────────────────────────────────────────
    T10   VXLAN 100% + multi-IP      (inner differs from T3)
    T10b  VXLAN 50% + multi-IP       (inner differs from T3)
    T11   full workload              (distrib -> T6 proxy estimate)
```

**testpmd is NOT a direct competitor.**  It measures the
`pre-built packet -> rte_eth_tx_burst() -> PMD -> NIC` path.
Bless measures the `mbuf alloc -> traffic model -> header construction ->
checksum -> VXLAN/mutation/sampler -> rte_eth_tx_burst() -> PMD -> NIC`
path.  Every delta in the ladder is a *cost attribution*, not a
"bless is X% slower than testpmd" comparison.

### 6.2 Environment

| Parameter | Value |
|-----------|-------|
| NIC | Mellanox ConnectX-6 (MT28908), PF @ `0000:00:04.0` |
| Link state | **NO-CARRIER** (no cable; link-down TX-path reference only) |
| PMD | mlx5_pci |
| DPDK | 23.11.3 (static link) |
| CPU | 40-core, dual-NUMA |
| lcore | 0 (main) + 1 (worker), same NUMA |
| TX queues | 1 |
| Burst size | 64 |
| Offload | None (T1, T3, T6) |
| bless commit | `a9cf965` (bench-template mode added) |

#### Frame Size Alignment

**Critical calibration note:** `testpmd --txpkts=N` and `bless imix=[N]`
produce DIFFERENT L2 frame sizes.  Failing to align them makes cross-tool
comparisons invalid.

| testpmd `--txpkts` | bless `imix` (UDP/TCP) | L2 frame | Wire occupancy |
|:---:|:---:|:---:|:---:|
| 64 | **50** | 64 B | 84 B |
| 78 | 64 | 78 B | 102 B |
| 128 | 114 | 128 B | 152 B |
| 512 | 498 | 512 B | 536 B |
| 1518 | 1504 | 1518 B | 1542 B |

**Rule:** `bless imix = testpmd_size - 14` (Ethernet header).

bless `imix[N]` specifies the **IP packet size** (L3 header + L4 header +
payload).  L2 frame = `imix + 14` (Ethernet header).  This is confirmed
empirically via the bench-template mode which logs `pkt_len`:

```
imix=[50]  -> pkt_len=64   (ETH 14 + IP 50)
imix=[64]  -> pkt_len=78   (ETH 14 + IP 64)
imix=[128] -> pkt_len=142
imix=[512] -> pkt_len=526
imix=[1500]-> pkt_len=1514
```

**T0 testpmd uses `--txpkts=78` to match Bless's 78-byte L2 frame produced
by `imix=[64]`.**  All tiers in §6 use the same configured L2 packet length
of 78 bytes (Ethernet FCS is appended by the NIC and not included in the
DPDK `pkt_len`).

### 6.3 Protocol

- **5 independent process restarts per tier** (up from 3 in v1)
- **Bless tiers (T1-T11):** `perf stat -p <pid>` attached post-stabilisation,
  matched 10 s window.  Packet counters from `/metrics` sampled at window
  start and end -> same measurement window for MPPS and per-packet metrics.
- **T0 (testpmd):** four-point bilateral protocol with
  `perf stat --delay=-1 --control=fifo:...`.  Packet-counter reads are
  taken before and after both the enable and disable boundaries:
  `pkt_start_before -> enable+ACK -> pkt_start_after -> [10 s window] ->
  pkt_end_before -> disable+ACK -> pkt_end_after`.  The packet count
  within the perf counting window is bounded by
  `[pkt_end_before-pkt_start_after,
  pkt_end_after-pkt_start_before]`, producing a complete bilateral
  inst/pkt bound (spread = (inst_upper-inst_lower)/inst_mid;
  median 0.56%, max 0.63% across 5 runs).
- Process-wide counting (all threads: worker + HTTP server + control-plane)
- `sample-interval=0` except T7/T8; `batch-delay-us=0`; `pps-rate=0`
- Median reported (not mean) with min-max range and per-run detail
- Runs labelled `T<N>-<run>` for traceability
- **Perf stat events**: `cycles,instructions,branches,branch-misses`
- **Hardware**: ConnectX-6 PF `0000:00:04.0`, NO-CARRIER (no link peer)
- **Building**: `make -j BUILD=release STATIC=1` (DPDK 23.11.3 static)

### 6.4 Results -- Hardware PF

*All data from 5 independent runs with matched 10 s measurement window.
Medians reported; spread is typically < 3% for inst/pkt and < 5% for MPPS.*

| Tier | Mode | MPPS | cyc/pkt | inst/pkt | IPC | bmiss% | br/pkt |
|------|------|------|---------|----------|-----|--------|--------|
| **T0** | testpmd txonly | 37.66 | 80.3 | 188.1 | 2.34 | 0.2% | 33 |
| **T1** | bless bench-template | 3.37 | 829 | 798 | 0.96 | 2.2% | 109 |
| **T3** | bless fixed-UDP (sw cksum) | 3.44 | 745 | 939 | 1.26 | 0.9% | 127 |
| **T4** | bless fixed-UDP (sw cksum explicit) | 3.43 | 749 | 942 | 1.26 | 0.9% | 127 |
| **T5** | bless fixed-UDP (hw offload) | 4.04 | 626 | 664 | 1.06 | 1.4% | 82 |
| **T6** | bless 3-protocol distrib | 3.29 | 765 | 906 | 1.18 | 1.1% | 125 |
| **T7** | bless distrib + sampler int=1 | 2.19 | 1,160 | 1,304 | 1.12 | 1.6% | 210 |
| **T8** | bless distrib + sampler int=64 | 2.85 | 919 | 1,036 | 1.13 | 2.9% | 146 |
| **T9** | bless fixed-UDP + mutation | 3.08 | 824 | 1,005 | 1.22 | 0.9% | 138 |
| **T9b**| bless distrib + mutation | 2.99 | 844 | 973 | 1.15 | 1.2% | 136 |
| **T10** | bless + VXLAN 100% (multi-IP) | 2.11 | 1,202 | 1,402 | 1.17 | 1.1% | 187 |
| **T10b**| bless + VXLAN 50% (multi-IP) | 2.35 | 1,084 | 1,227 | 1.13 | 1.6% | 163 |
| **T10c**| bless + VXLAN 50% (single-IP) | 2.34 | 1,092 | 1,232 | 1.13 | 1.6% | 164 |
| **T11**| bless full workload† | 1.99 | 1,293 | 1,399 | 1.08 | 3.7% | 194 |

†T11 = distrib + sampler int=64 + mutation threshold=100 + VXLAN 50%.
All tiers use the same configured L2 packet length of 78 bytes
(Bless `imix=[64]`, testpmd `--txpkts=78`).  T0 is measured with
a four-point bilateral protocol: packet-counter reads bracketing
both the enable and disable boundaries, producing a complete bilateral
inst/pkt bound (spread = (inst_upper-inst_lower)/inst_mid, <=0.63%
across all runs, median 0.56%).  MPPS is also the midpoint
of its bilateral range.
T3/T4/T5/T9/T10c share single-IP inner; T6/T7/T8/T9b
share 3-protocol multi-IP inner; T10/T10b/T11 share multi-IP inner.
*All values are process-wide (all threads), not hot-path only.*

### 6.5 Cost Decomposition

**Clean feature deltas** (tiers share the same inner config; diff =
incremental feature cost on HW PF, process-wide):

| Delta  | Feature measured | inst/pkt | cyc/pkt |
|:---|------|:---:|:---:|
| T4 - T3 | sw cksum explicit (noise floor) | +3 | +4 |
| T5 - T4 | **HW checksum offload** | **-278** | -123 |
| T7 - T6 | **sampler int=1** | **+398** | +395 |
| T8 - T6 | **sampler int=64** | **+130** | +154 |
| T9 - T3 | **mutation (fixed-UDP inner)** | **+66** | +79 |
| T9b - T6 | **mutation (distrib inner)** | **+67** | +79 |
| T10c - T3 | **VXLAN 50% (single-IP inner)** | **+293** | +347 |

**Not clean (different inner configs):**

| Delta  | Feature measured | inst/pkt | Why not clean |
|:---|------|:---:|------|
| T6 - T3 | distrib + multi-IP | -33 | multi-IP range differs |
| T10 - T3 | VXLAN 100% + multi-IP | +463 | multi-IP inner + VXLAN |
| T10b - T3 | VXLAN 50% + multi-IP | +288 | multi-IP inner + VXLAN |

**Key observations from clean deltas:**

- **Mutation cost is consistent across the two tested inner
  configurations:** T9-T3 = +66, T9b-T6 = +67 -- identical within
  measurement noise.  This supports treating mutation as approximately
  separable across these two tested inner configurations, but does not
  prove a general model (only two inner configs, one threshold, one
  mutation class were tested).
- **VXLAN 50% clean delta (+293) vs multi-IP T10b (+288):** T10b and
  T10c differ by only 5 inst/pkt, which is below the observed
  measurement spread.  This shows no detectable total-cost difference
  between the single-IP and multi-IP VXLAN configurations, but does
  not isolate the standalone multi-IP iteration cost.
- **T10c provides the clean VXLAN baseline** that T10b could not:
  single-IP inner shared with T3, VXLAN 50% as the only change.

**Updated first-order additive estimate** for T11 (distrib + sampler
int=64 + mutation threshold=100 + VXLAN 50%):

| Component | Source | inst/pkt |
|:---|:---|:---:|
| Baseline (T6: distrib) | measured | 906 |
| + sampler int=64 | T8 - T6 (clean) | +130 |
| + mutation threshold=100 | T9b - T6 (clean) | +67 |
| + VXLAN 50% | T10c - T3 (single-IP clean, ->T6 proxy) | +293 |
| **Predicted total** | | **1,396** |
| **Measured (T11)** | | **1,399** |
| **Error** | | **+0.2%** |

Three of four components now use clean deltas from matching inner
configs.  The VXLAN term remains a cross-config proxy (T10c is
fixed-UDP, T11 is distrib), but the multi-IP confound has been
eliminated.  Error cancellation cannot be ruled out.

**Key findings revised (v2, matched-window methodology):**

1. **The old methodology systematically undercounted inst/pkt.**  The
   prior approach used process-lifetime perf stat (including EAL init,
   config parsing, and idle threads) but a 2 s tail window for PPS.
   T1 went from 302->798 inst/pkt; T3 from 1,094->939; T11 from
   1,548->1,399.  All prior data is invalidated.

2. **The template-copy baseline has moderate cost.**  T1 (bench-template:
   mbuf alloc + template memcpy + TX burst) costs 798 inst/pkt, with an IPC of
   0.96.  This value includes mbuf allocation and template memcpy and
   should not be interpreted as framework-only overhead.  The old claim
   of "framework is < 5% of testpmd" was an artefact of the methodology
   error; T0 testpmd under the matched-window methodology measures 188
   inst/pkt (24% of T1).  **T0 is not an allocation-and-copy-equivalent
   baseline:** testpmd txonly recycles pre-allocated mbufs in a simple
   forwarding loop, while Bless template mode includes fresh mbuf alloc,
   template memcpy, and metadata preparation.  The 188->798 gap reflects
   the full lifecycle difference, not just framework overhead.

3. **Header construction delta is +141 inst/pkt (net, not absolute).**
   T3 uses 141 more instructions per packet than T1.  This is the net
   incremental cost of replacing the template memcpy path with dynamic
   fixed-UDP field construction -- it is not the standalone cost of
   "header construction."  T1 and T3 are fundamentally different code
   paths (template copy vs field-by-field building), not T1 + builder.
   The old methodology over-attributed this delta (+792 vs +141) because
   it spread T1's own cost across the full process lifetime.

4. **HW checksum offload is a large win.**  T4->T5 saves -278 inst/pkt
   (-29%) and boosts MPPS from 3.43->4.04 (+18%).  The old data claimed
   T4~T5; the difference was lost in the methodology noise.

5. **Sampler interval=1 is the largest clean single-variable cost.**
   Among the clean single-variable deltas (tiers sharing the same inner
   config), T7 (sampler int=1) adds +398 inst/pkt -- the largest measured
   clean per-feature cost.  The nominal VXLAN delta (+463, T10-T3) is
   larger but includes a different multi-IP inner configuration and is
   not directly comparable.  At int=64 the sampler cost drops to +130
   inst/pkt.  The branch-miss rate rises to 2.9% in T8; the available
   counters cannot attribute this to specific branch patterns.

6. **VXLAN overhead is substantial but not the dominant cost.**  T10b
   (VXLAN 50%) adds +288 inst/pkt vs T3.  Partial-ratio VXLAN is
   non-proportional: 50% costs +288 vs 1/2x463 = +231.5 (+24% above
   linear), consistent with the taken/not-taken branch mix at 50%.

7. **Branch prediction impact is measurable but not dominant.**
   T11 has 3.7% bmiss vs 0.9% for T3 -- four mixed or conditional
   paths that may contribute to branch misses (protocol dispatch,
   sampler guard, mutation guard, VXLAN ratio).  However, IPC in T11
   (1.08) remains above 1.0, suggesting the branches are well-predicted
   on average.  The bmiss rate alone does not quantify cycle impact;
   branches/pkt and misprediction penalty are needed for proper
   attribution.

### 6.6 net_null Comparison

*Prior net_null data is invalid (same measurement-window mismatch).
Pending re-measurement under the new matched-window methodology.*

### 6.7 Usage

```bash
# All tiers use the ladder_tier.py script with matched-window methodology:
#   python3 tools/ladder_tier.py <config> <label>
# Runs 5 independent restarts, reports median + min/max.

# Clean/cross-config check (no measurement) — diff two configs and get
# a verdict before running.  Use this to confirm a new tier isolates a
# single feature (clean) rather than changing the workload shape
# (cross-config).  See §6.1 for the rules.
#   python3 tools/ladder_tier.py <config> <label> \
#       --vs conf/config-t3-fixed-udp.yaml --diff-only

# T1: bench-template (measure the minimum template-copy TX path)
python3 tools/ladder_tier.py conf/config-bench.yaml T1 --bench-mode template

# T3: fixed-UDP dynamic construction
#     (compare dynamic construction against the T1 template-copy path)
python3 tools/ladder_tier.py conf/config-t3-fixed-udp.yaml T3

# T4/T5: sw vs hw checksum
python3 tools/ladder_tier.py conf/config-t4-sw-cksum.yaml T4
python3 tools/ladder_tier.py conf/config-t5-hw-offload.yaml T5

# T6: 3-protocol distribution
python3 tools/ladder_tier.py conf/config-t6-distrib.yaml T6

# T7/T8: sampler cost
python3 tools/ladder_tier.py conf/config-t7-sampler1.yaml T7
python3 tools/ladder_tier.py conf/config-t8-sampler64.yaml T8

# T9: mutation cost (fixed-UDP)
python3 tools/ladder_tier.py conf/config-t9-mutation.yaml T9

# T9b: mutation cost (distrib inner, clean delta vs T6)
python3 tools/ladder_tier.py conf/config-t9b-mutation-distrib.yaml T9b

# T10/T10b: VXLAN overhead (multi-IP inner)
python3 tools/ladder_tier.py conf/config-t10-vxlan.yaml T10
python3 tools/ladder_tier.py conf/config-t10b-vxlan50.yaml T10b

# T10c: VXLAN 50% (single-IP inner, clean delta vs T3)
python3 tools/ladder_tier.py conf/config-t10c-vxlan50-singleip.yaml T10c

# T11: full workload
python3 tools/ladder_tier.py conf/config-t11-full.yaml T11

# T0: testpmd baseline (--txpkts=78 matches Bless imix=[64] -> 78B L2)
python3 tools/t0_testpmd.py
```

### 6.8 Next Tiers

All bless tiers T0-T11 + T9b + T10c measured with corrected
matched-window methodology.  T0 remeasured 2026-07-19 (v3) with a four-point bilateral
protocol: packet-counter reads bracketing both enable and disable
boundaries.  The complete bilateral bound spread (= (inst_upper-inst_lower)/inst_mid)
is <=0.63% (median 0.56%) across 5 runs.  `--txpkts=78` matches Bless's 78-byte L2 frame.

### 6.9 Checksum Overhead (T4/T5)

Software vs hardware checksum on HW PF (fixed-UDP, single IP):

| Tier | Mode | MPPS | cyc/pkt | inst/pkt | IPC |
|------|------|------|---------|----------|-----|
| T4 | sw cksum (rte_ipv4_cksum) | 3.43 | 749 | 942 | 1.26 |
| T5 | hw offload (mbuf OL flags) | 4.04 | 626 | 664 | 1.06 |
| **Delta ** | | **+18%** | **-123** | **-278** | -- |

**Hardware checksum offload saves -278 inst/pkt (-29%) in this
single-core mlx5 NO-CARRIER configuration.** Under the old (pre-v2)
methodology this difference was invisible (claimed Delta  = -5 inst/pkt).
The old methodology's process-lifetime perf stat diluted the hot-path
difference with EAL init and idle-thread instructions.

IPC decreases from 1.26 to 1.06 because instruction count falls
proportionally more than cycle count; the current counters do not
identify which remaining stalls dominate the cycle budget.

### 6.10 Entropy Sampler Cost (T7/T8)

Sampler overhead on HW PF (3-protocol distrib: TCP:UDP:SCTP = 1:3:1),
varying `sample-interval`:

| Interval | MPPS | cyc/pkt | inst/pkt | bmiss% | Delta  inst vs T6 |
|----------|------|---------|----------|--------|:---:|
| 0 (T6) | 3.29 | 765 | 906 | 1.1% | -- |
| 1 (T7) | 2.19 | 1,160 | 1,304 | 1.6% | **+398** |
| 64 (T8) | 2.85 | 919 | 1,036 | 2.9% | **+130** |

The sampler calls `entropy_extract_5tuple()` for **every** packet when
`sample-interval > 0` -- extraction is NOT rate-limited, only the
ring-buffer write is.  This means:

- **interval=1:** full cost (+398 inst/pkt) -- extraction + ring write
  every packet.  MPPS drops 33%.
- **interval=64:** extraction every packet, ring write 1/64th (+130
  inst/pkt).  Branch-miss rate rises to 2.9%.  The higher rate is
  correlated with enabling the interval guard and periodic ring-write
  path; the available counters do not show whether the misses occur
  specifically on every 64th iteration.

**Recommendation:** Use `sample-interval >= 64` for production traffic
generation.  `sample-interval=1` is only appropriate for entropy
debugging.

Entropy aggregation runs on the main core, but the TX worker still
performs 5-tuple extraction and ring publication for every sampled
packet.  Aggregation is therefore offloaded, not free; cross-core
communication and cache effects may still affect TX throughput.

### 6.11 Mutation Cost (T9)

Mutation overhead on HW PF (fixed-UDP, `ipv4:version` class,
threshold=100/1024 ~ 9.8%):

| Tier | MPPS | cyc/pkt | inst/pkt | bmiss% | Delta  inst vs T3 |
|------|------|---------|----------|--------|:---:|
| T3 (no mutation) | 3.44 | 745 | 939 | 0.9% | -- |
| T9 (threshold=100) | 3.08 | 824 | 1,005 | 0.9% | **+66** |

Mutation cost is dominated by the guard check (`fast_rand_next() &
1023 < threshold`).  At threshold=100 (~9.8% trigger rate), the
incremental cost is +66 inst/pkt (+7% of baseline).  The branch-miss
rate is unchanged (0.9%) -- the guard branch is highly predictable at
~10% trigger rate (90% not-taken).

**Branch unpredictability is expected to peak near a 50% trigger
probability (threshold ~ 512), while ratios near 0 or 1023 should be
easier to predict.**  A full threshold sweep is required to verify
this on the current CPU.  The current single data point (threshold=100)
is not sufficient to characterise the predictor behaviour.

#### T9b -- Mutation on Distrib Inner

| Tier | MPPS | inst/pkt | bmiss% | Delta  inst vs T6 |
|------|------|----------|--------|:---:|
| T6 (no mutation) | 3.29 | 906 | 1.1% | -- |
| T9b (threshold=100) | 2.99 | 973 | 1.2% | **+67** |

T9b uses the same 3-protocol multi-IP inner as T6, providing a clean
delta: **mutation costs +67 inst/pkt on distrib** -- identical to the
+66 measured on fixed-UDP (T9-T3) within measurement noise.  This
supports treating mutation as approximately separable across these
two tested inner configurations.

### 6.12 VXLAN Encapsulation Overhead (T10 / T10b / T10c)

VXLAN outer header cost at 100% and 50% ratio, HW PF (fixed-UDP inner,
**64-IP range** -- not clean Delta  vs T3 which uses single IP):

| Metric | T3 (no VXLAN, single IP) | T10b (50%) | T10 (100%) |
|--------|:---:|:---:|:---:|
| MPPS | 3.44 | 2.35 | 2.11 |
| inst/pkt | 939 | 1,227 | 1,402 |
| Delta  inst vs T3 | -- | **+288** | +463 |
| branch-miss | 0.9% | 1.6% | 1.1% |

**Important:** T10/T10b use `src: "10.0.0.1+64"` (64-IP range) while
T3 uses `src: "10.0.0.1"` (single IP).  The Delta  includes the multi-IP
iteration loop and is **not** a clean VXLAN-only cost.  A clean VXLAN
tier (single-IP inner + VXLAN) is needed for precise attribution.

**Partial-ratio VXLAN is non-proportional.** 50% costs +288 vs
1/2x463 = +231.5 (+24% above linear).  At 50% ratio, the
`fast_rand()` guard and alternating VXLAN/non-VXLAN code paths add
overhead not present at 100% (where the branch at 1.1% bmiss is
trivially predictable).

**Outer IP format note:** VXLAN IPv4 source addresses use the
`IP:VNI+range` syntax (e.g. `172.16.0.1:100+10`).  Destination
addresses are plain IPs -- the VNI is carried only on the source side.

**VXLAN Delta  sources (not decomposed).** The +288-463 inst/pkt includes:
outer-header construction, packet prepend/shift, additional memory
writes, larger transmitted frame, potentially changed PMD behaviour,
the multi-IP iteration loop, and the `fast_rand()` guard.  `perf stat`
does not provide function-level breakdown -- `perf record` is required.

#### T10c -- Clean Single-IP VXLAN 50%

| Metric | T3 (no VXLAN) | T10c (single-IP, 50%) |
|--------|:---:|:---:|
| MPPS | 3.44 | 2.34 |
| inst/pkt | 939 | 1,232 |
| Delta  inst vs T3 | -- | **+293** |
| branch-miss | 0.9% | 1.6% |

T10c shares T3's exact single-IP inner config (`src: "10.0.0.1"`), so
the +293 inst/pkt is a **clean VXLAN 50% cost** -- no multi-IP iteration
confound.  Compared to T10b (+288 on multi-IP), the difference is
within measurement noise (~5 inst/pkt).  No meaningful aggregate-cost
difference is observed between the single-IP and multi-IP VXLAN
configurations; the standalone multi-IP loop cost is not isolated by
this comparison.

### 6.13 Full Workload (T11)

Combined workload on HW PF (3-protocol distrib + sampler int=64 +
mutation ratio=10 + VXLAN 50%):

| Metric | T11 | vs T6 (distrib) |
|--------|:---:|:---:|
| MPPS | 1.99 | -40% |
| cyc/pkt | 1,293 | +528 |
| inst/pkt | 1,399 | +493 |
| IPC | 1.08 | -- |
| branch-miss | 3.7% | +2.6pp |
| branches/pkt | 194 | -- |

The additivity estimate (§6.5) predicts 1,396 inst/pkt -- within 0.2%
of measured 1,399.  See §6.5 for the full decomposition and caveats.

**Branch prediction impact:** T11 has 3.7% bmiss vs 0.9% for T3 --
four mixed or conditional paths that may contribute to branch misses
(protocol dispatch, sampler guard, mutation guard, VXLAN ratio
selection).  IPC remains above 1.0, but IPC alone cannot determine how
well these individual branches are predicted.  The bmiss rate alone does
not quantify cycle impact -- branches/pkt (=194) and per-architecture
misprediction penalty are needed for proper attribution.  Impact under
link-up conditions remains to be measured.

### 6.14 Known Gaps

The gaps below are the ladder's pending feature-isolation queue: each row
is a candidate clean tier to add when a change cannot be attributed by
the existing tiers (see the trigger criteria in Section 6.1).  A gap
becomes a new tier by fixing every inner-config variable except the one
under test.

| Gap | What | Why it matters |
|-----|------|----------------|
| **Methodology** | v2 (2026-07-18) for T1-T11; v3 (2026-07-19) for T0 with four-point bilateral protocol | Prior data invalid due to measurement-window mismatch (§6.3) |
| **Link-up** | All data is NO-CARRIER | Bottleneck profile likely differs; actual limiting factor remains to be measured |
| **Cross-config VXLAN proxy** | T10c VXLAN delta from fixed-UDP, T11 uses distrib inner | Only remaining cross-config term in additivity; VXLAN-on-distrib tier needed |
| **Mutation sweep** | Only threshold=100/1024 (~9.8%), single class | Full threshold sweep (0-1023) + all mutation classes unmeasured |
| **VXLAN ratio sweep** | Only 50% and 100% measured | Partial ratios (25%, 75%) needed to characterise non-proportionality |
| **Different protocol mixes** | Only 1:3:1 TCP/UDP/SCTP tested | Other distributions may change sampler interaction costs |
| **Worker vs process** | Process-wide perf stat (all threads) | Hot-path worker cost may differ from system-wide; worker-thread targeting needed |
