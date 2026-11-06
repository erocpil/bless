# Three-Layer Device Comparison: net_null vs ConnectX-6 PF vs VF

This document records a three-way throughput and per-packet-cost comparison
across the three DPDK TX paths available to bless on this host.  It answers one
question  --  *what does each device layer contribute to the TX path?*  --  by
measuring the same workload against a software drop (net_null), a physical
function (ConnectX-6 PF), and a virtual function (ConnectX-6 VF).

The motivating result is counter-intuitive and is explained in
[Result analysis](#result-analysis): the software drop is **not** the fastest
layer, and the SR-IOV virtual function is **not** measurably slower than the
physical function.

## Environment

| Property | Value |
|----------|-------|
| CPU | Intel Xeon Gold 6148 @ 2.40 GHz, 40 vCPUs, 2 sockets x 20 cores |
| Hypervisor | KVM/QEMU guest (`isolcpus=0-16 nohz_full=0-16 rcu_nocbs=0-16`) |
| NUMA | node0 = 0-19, node1 = 20-39 |
| Kernel | 6.1.0-37-amd64 |
| DPDK | 23.11.3 (`/opt/dpdk`) |
| NIC | ConnectX-6 (MT28908): PF `0000:00:04.0` (15b3:101b) + 6 VFs `00:05.0`-`00:0a.0` (15b3:101c) |
| Management NIC | VirtIO `00:03.0` (1af4:1000) |
| Hugepages | 32 x 1 GB (31 free at test time) |
| E-switch | legacy (encap-mode basic) |
| bless | main @ `0da5b2e`+, release-static |

**Safety.** The PF carries NO-CARRIER (no physical link partner), so packets
are discarded at the PHY layer and cannot reach the wire.  The VF link inherits
the PF's link-down state, so its TX also terminates at the e-switch/PHY.  All
results below measure the TX descriptor path (software construction + PCIe +
PMD), never wire-rate throughput.

## Design

### Three layers, one workload

The three layers differ **only** in the `dpdk:` config section; the workload
(inner config) is identical:

| Layer | `dpdk:` | PMD | TX path |
|-------|---------|-----|---------|
| T-soft | `vdev: net_null0` + `no-pci`/`no-huge`/`no-shconf` | net_null | software drop + synchronous mbuf free |
| T-pf | `allow: "0000:00:04.0"` | mlx5_pci | PCIe DMA + NIC descriptor |
| T-vf | `allow: "0000:00:05.0"` | mlx5_pci | PCIe DMA + e-switch forward |

Unified inner config:

```yaml
injector:
  batch: 128
  batch-delay-us: 0
  sample-interval: 0        # entropy sampler off
  mode: tx-only
  num: -1                   # unbounded; SIGTERM terminates
  udp: 100                  # single protocol
bless:
  hw-offload: []
  ether:
    copy-payload: false     # no payload memcpy
    imix: [64]              # fixed 64-byte IP packet -> 78-byte L2 frame
    dst: "02:00:00:00:00:01"
    type: { ipv4: { src: "10.0.0.1+64", dst: ["192.168.1.1"], udp: {...} } }
  vxlan: { enable: false }
  erroneous: { ratio: 0 }
```

Configs live in `conf/bench-3layer-{soft,pf,vf}.yaml`.

### Three metric dimensions

A device layer is characterized by three non-interchangeable dimensions
(per the [benchmark methodology](benchmarks.md) three-benefit rule):

1. **Throughput (MPPS)**  --  `dpdk_opackets` delta rate, cross-checked against
   `bless_tx_mpps`.  What the worker sustains end-to-end.
2. **Synchronous per-packet timing (cycles/pkt)**  --  `tx_build_cycles_per_pkt`
   (construction) and `tx_submit_cycles_per_pkt` (`rte_eth_tx_burst` duration)
   from `/api/stats` -> `observe`.
3. **Instruction cost (inst/pkt)**  --  matched-window `perf stat` on the TX worker
   thread, with IPC and branch-miss/pkt as secondary.

Deltas attribute cost:

- `T-pf - T-soft` = hardware TX path vs software drop (net of the net_null free
  overhead  --  see below).
- `T-vf - T-pf` = SR-IOV + e-switch forwarding cost.

## Execution

### Step 0  --  VF probe

Before any full run, a bounded VF probe (single worker, `-T 1`,
`--num 50000000`) confirmed the VF could be driven at all: `EAL: Probe PCI
driver: mlx5_pci (15b3:101c) device: 0000:00:05.0`, `dpdk_opackets`
incrementing (16.4M -> 24.9M -> 33.5M -> 41.3M -> 49.9M at 1 s intervals),
steady-state `bless_tx_mpps ~ 7.8 MPPS`, a clean exit, and a single
informational `mlx5_core ... ens5: Link down` in `dmesg` (no errors).

### Step 1  --  Throughput

```bash
python3 tools/bench_three_layer.py
```

5 independent restarts per layer, interleaved ABCABC to cancel environmental
drift.  Each run: warmup 5 s, then 5 samples of `dpdk_opackets` and
`bless_tx_mpps` at 2 s intervals, terminated by SIGTERM.  Throughput = median
of the per-interval `dpdk_opackets` delta rates.

### Step 2  --  Per-packet instruction cost

```bash
python3 tools/bench_three_layer_inst.py
```

5 restarts per layer, interleaved ABCABC.  Each run:

1. Launch bless, warmup 5 s.
2. Identify the TX worker thread by CPU affinity  --  the thread pinned to CPU 1
   (`Cpus_allowed_list: 1`, `comm=tx_only@1`), distinct from the main lcore
   (CPU 0) and the civetweb server threads (CPUs 17-39).
3. Record `pkt_start` (`dpdk_opackets`).
4. `perf stat -e instructions,cycles,branches,branch-misses -t <worker_tid>
   --timeout 10000`.
5. Record `pkt_end`; `inst/pkt = instructions / (pkt_end - pkt_start)`.

The worker-thread scope excludes the HTTP server and control lcore, whose
instruction counts would otherwise dilute the hot-path measurement.

## Results

### Throughput (5-restart median, `dpdk_opackets` delta rate)

| Layer | MPPS median | min-max | `bless_tx_mpps` (cross-check) |
|-------|-------------|---------|-------------------------------|
| T-soft (net_null) | **6.689** | 6.533-6.855 | 6.527 |
| T-pf (ConnectX-6 PF) | **7.987** | 7.787-8.180 | 7.791 |
| T-vf (ConnectX-6 VF) | **7.795** | 7.785-8.179 | 7.790 |

| Delta | Value |
|-------|-------|
| `T-pf - T-soft` | +1.298 MPPS (**+19.4%**) |
| `T-vf - T-pf` | -0.191 MPPS (-2.4%, inside 5-round spread) |

### Synchronous per-packet timing (single-run `/api/stats` read)

| Metric (cycles/pkt) | net_null | PF | VF |
|---------------------|----------|----|----|
| tx_build (construction) | 281.97 | 283.09 | 283.18 |
| **tx_submit (`rte_eth_tx_burst`)** | **78.64** | **20.57** | **20.42** |
| tx_cycles (total) | 360.61 | 303.65 | 303.61 |

### Instruction cost (5-restart median, worker-thread perf)

| Layer | inst/pkt | IPC | branch/pkt | branch-miss/pkt |
|-------|----------|-----|-----------|-----------------|
| T-soft | 892.6 | 1.91 | 121.8 | 2.02 |
| T-pf | 906.9 | 2.27 | 122.7 | 1.07 |
| T-vf | 864.2 | 2.27 | 116.9 | 1.02 |

| Delta (inst/pkt) | Value |
|------------------|-------|
| `T-pf - T-soft` | +14.3 (**+1.6%**) |
| `T-vf - T-pf` | -42.7 (**-4.7%**, direction stable across all 5 rounds) |

## Result analysis

### 1. net_null is not a free drop (why T-soft < T-pf)

The software drop is ~19% **slower** than the physical NIC.  The cause is in
the TX burst itself: `tx_submit_cycles_per_pkt` is 78.64 on net_null vs 20.57
on mlx5.  net_null's `rte_eth_tx_burst` synchronously frees every mbuf back to
the mempool, a cache-miss-heavy operation, whereas mlx5 batches a doorbell
write and reclaims mbufs asynchronously after DMA completion.  This is visible
in IPC (1.91 vs 2.27): net_null stalls on the mempool free, mlx5 does not.

Consequence for the [Reference Ladder](benchmarks.md): the net_null tier does
**not** isolate pure packet construction.  It measures construction **plus** the
net_null free overhead.  The true construction cost is
`tx_build_cycles_per_pkt` ~ 282 cycles/pkt, identical across all three layers
(281.97 / 283.09 / 283.18), confirming the unified inner config.

### 2. SR-IOV + e-switch is not measurable in throughput

`T-vf - T-pf = -2.4%` sits inside the 5-round spread (PF 7.787-8.180 vs VF
7.785-8.179, no consistent round-wise direction), and `tx_submit` is
20.57 vs 20.42 cycles/pkt.  In TX-only + NO-CARRIER mode the e-switch forwarding
cost does not register on throughput or synchronous timing.  It is **not
resolvable from the noise**  --  not "zero overhead".

### 3. The VF worker runs fewer instructions per packet

`T-vf - T-pf = -4.7%` inst/pkt, with the direction stable across all 5 rounds
(VF below PF every round).  The magnitude should be read cautiously: the PF
series shows run-to-run spread (879.9-965.6 inst/pkt) far larger than the VF
series (863.0-889.4), so the -4.7% median is bounded by that noise.  The
direction, however, is consistent and plausible: on a VF the doorbell and
completion are handled by firmware/e-switch outside the worker's instruction
stream, so the worker executes fewer instructions per packet even though
aggregate throughput is not higher.

The three dimensions together say: the VF trades a small worker-side
instruction saving for a small, statistically-unresolvable throughput penalty  -- 
consistent with e-switch forwarding being offloaded out of the worker.

## Conclusion

| Question | Answer |
|----------|--------|
| Which layer is fastest? | PF ~ VF (7.99 vs 7.80 MPPS, not resolvable); both exceed net_null (6.69 MPPS) |
| Is net_null the software ceiling? | No  --  its synchronous mbuf free makes it ~19% slower than hardware |
| What does SR-IOV cost? | Not measurable in throughput or timing; -4.7% inst/pkt (direction stable, magnitude inside PF noise) |

## Bidirectional (tx-rx) extension

The comparison above is TX-only.  Extending it to a bidirectional tx-rx mode
(to enable handshake, latency, or wire-rate PPS) was explored and is blocked
by three host constraints:

- **Fixed-off loopback**: both PF and VF report `loopback: off [fixed]`; the
  firmware disables internal loopback and ethtool cannot enable it.
- **NO-CARRIER**: the PF/VF have no physical link, so their RX queues receive
  no traffic.
- **Legacy e-switch does not switch between VFs**: with the hypervisor's
  legacy e-switch, VF traffic is forwarded to the uplink instead of being
  switched locally between VFs; under NO-CARRIER the uplink drops it silently
  (no dmesg errors).  Verified by VF0 tx-only -> VF1 rx-only: 1000 packets
  sent, 0 received, on two VF pairs.

Consequences:

- Handshake (a bidirectional state machine) cannot run on any of the three
  layers, since all three require RX that is unavailable here.
- `fwd` mode is an L2 single-port MAC-swap reflector, not a TCP state
  machine, so it cannot substitute for handshake.

Enabling a tx-rx path requires either a host-side e-switch change (switchdev
plus a representor and bridge, or legacy local switching) or a software
baseline via veth + PCAP to validate `fwd` reflection and RTT accounting
first.

## Known limitations

- **TX-only**: no RX, no latency, no wire-rate PPS; the blocked tx-rx
  extension is covered in "Bidirectional (tx-rx) extension" above.
- **Single worker, single TX queue**: multi-queue scaling (where the legacy
  e-switch doorbell ceiling binds) is not tested.
- **timing columns are single-run reads**, not 5-restart statistics; the
  58-cycle submit gap is far outside noise, but treat the exact figures as
  indicative.
- **PF inst/pkt has high run-to-run spread** (879.9-965.6); the -4.7% VF delta
  direction is stable but its magnitude shares that uncertainty.
- **Passthrough VF is not NUMA-aware** (`EAL: Device ... is not NUMA-aware`); no
  locality tuning was possible or attempted.

## Reproducing

```bash
cd /root/src/bless
python3 tools/bench_three_layer.py          # throughput (Table 1)
python3 tools/bench_three_layer_inst.py     # inst/pkt (Table 3)
```

Both use `conf/bench-3layer-{soft,pf,vf}.yaml` and require the PF/VF to be
bound to `mlx5_core` with the interface down (bifurcated model, no vfio-pci).
