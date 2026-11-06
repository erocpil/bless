# CPU Profiling Guide

This guide describes how to profile BLESS using `perf` and generate
flame graphs for performance analysis.

## Prerequisites

```bash
apt-get install linux-perf linux-tools-$(uname -r)
git clone https://github.com/brendangregg/FlameGraph /opt/FlameGraph
```

## Quick Start

```bash
# 1. Start bless in background
bless conf/bench-udp64.yaml &

# 2. Record 30 seconds of CPU samples at 99 Hz
perf record -F 99 -g -p $(pidof bless) -- sleep 30

# 3. Generate flame graph
perf script | /opt/FlameGraph/stackcollapse-perf.pl > perf.folded
/opt/FlameGraph/flamegraph.pl perf.folded > bless-flame.svg
```

Open `bless-flame.svg` in a browser.  Wider bars = more CPU time.

## Targeted Profiling Recipes

### Cache Miss Analysis

```bash
perf stat -e cache-misses,cache-references,instructions,cycles \
  -p $(pidof bless) -- sleep 30
```

### Branch Prediction

```bash
perf stat -e branch-misses,branches \
  -p $(pidof bless) -- sleep 30
```

### NUMA Remote Access

```bash
perf stat -e node-loads,node-load-misses,node-stores,node-store-misses \
  -p $(pidof bless) -- sleep 30
```

### IPC (Instructions Per Cycle)

IPC below 1.0 on a modern core suggests memory stalls.
IPC above 3.0 suggests well-optimised compute-bound code.

```bash
perf stat -e instructions,cycles -p $(pidof bless) -- sleep 30
```

## Key Hotspot Functions

| Function | File | Expected % | Notes |
|----------|------|-----------|-------|
| `worker_func_tx_only` | worker.c | ~30-50% | Main TX loop |
| `bless_mbufs_udp` | bless.c | ~10-20% | UDP packet construction |
| `rte_eth_tx_burst` | DPDK | ~5-15% | Hardware TX |
| `bless_encap_vxlan` | bless.c | ~0-15% | VXLAN encap (if enabled) |
| `fast_rand_next` | define.c | ~2-5% | PRNG |
| `token_bucket_consume` | token_bucket.c | ~2-5% | Rate limiter |
| `bless_mbufs_tcp` | bless.c | ~0-10% | TCP packet construction |
| `entropy_sampler_should_sample` | entropy.h | ~0-3% | Sampling (interval-dependent) |

## Interpretation

### Narrow, squat flame = compute-bound

The TX loop (`worker_func_tx_only`) dominates.  Most time is spent
constructing packet headers and calling `rte_eth_tx_burst`.  This is
expected for a traffic generator.

### Wide, jagged flame = branchy / unpredictable

Many small functions with similar widths suggest unpredictable control
flow.  Check `fast_rand_next` and `distribute` for potential branch
misprediction.

### Tall tower = deep call stack

`bless_encap_vxlan` calling through IP/UDP/VXLAN layers adds stack
depth.  Not a problem unless the tower accounts for >20% of time.

## System-Level Profiling

Profile the entire system (not just bless) to find bottlenecks outside
the application:

```bash
perf record -F 99 -a -g -- sleep 30
perf script | /opt/FlameGraph/stackcollapse-perf.pl > perf-all.folded
/opt/FlameGraph/flamegraph.pl perf-all.folded > all-flame.svg
```

Look for:
- `mlx5*` / `i40e*` driver functions consuming CPU (NIC driver overhead)
- `__netif_receive_skb` / `ip_rcv` (kernel stack -- shouldn't appear, DPDK bypasses it)
- `rte_kni` (KNI overhead if KNI is in use)

## PMU Counter Reference

| Counter | What It Means | Healthy Range |
|---------|--------------|---------------|
| `instructions` | Total instructions retired | -- |
| `cycles` | Total CPU cycles | -- |
| IPC = insns/cycles | Efficiency | > 1.5 for network code |
| `cache-misses` | L1/L2/L3 misses | < 5% of references |
| `branch-misses` | Mispredicted branches | < 2% of branches |
| `node-load-misses` | Remote NUMA reads | < 1% of loads |

## Measured Flame Graphs

The two committed flame graphs
([net_null](../bless_flame_net_null.svg),
[PF](../bless_flame_hw_pf.svg)) capture Bless's hot path on the software drop
and the ConnectX-6 PF.  Both were recorded with a multi-protocol config and
the entropy sampler enabled, so their exact percentages are not comparable to
the single-protocol UDP figures in the
[three-layer comparison](../reference/three-layer-comparison.md).

Packet construction dominates both graphs.  `worker_func_tx_only` plus the
`bless_mbufs*` constructors take most of the samples: net_null 27.6% +
17.0% + 6.7% (UDP), PF 50.6% + 33.2% + 13.0% (UDP).  TX submission is
secondary.  The two graphs differ in how submission is done:

- net_null shows `rte_pktmbuf_free` at 3.0%: the synchronous per-packet mbuf
  return.
- PF shows `mlx5_tx_burst_sc_empw` at 3.9%: the batched doorbell submit.

This is the flame-graph evidence for the three-layer finding that net_null's
"free drop" carries real cost, while mlx5 submits in bulk.
