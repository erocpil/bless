# Pacing Backend Design

## Purpose and boundary

Bless currently controls how many packets are submitted in a burst and how
long software waits between bursts.  It does not guarantee when each packet
leaves the physical port.  A pacing backend separates workload timing semantics
from the mechanism used to submit or schedule packets.

```text
traffic model -> absolute deadlines -> pacing backend -> PMD/NIC -> wire
```

The traffic model decides when traffic should be sent.  The backend probes the
device, converts clocks, submits packets, applies the late policy, and reports
what actually happened.  Worker code must not contain mlx5/ice device tests.

## Time model

Three timestamps must remain distinct:

| Timestamp | Meaning |
|-----------|---------|
| `target_time` | Desired departure deadline produced by the traffic model |
| `submit_time` | Time software handed the burst or packet to the PMD |
| `wire_time` | Time the NIC transmitted the packet on the physical link |

`submit_time - target_time` is software submission error.  It is the only
quantity available to the current software path.  `wire_time - target_time` is
wire error and requires a trustworthy NIC egress timestamp or an independent
measurement device.  Descriptor completion and NIC enqueue timestamps must not
be presented as wire timestamps.

Deadlines are absolute.  The traffic model advances `next_deadline` by each
sampled interval instead of sleeping for that interval. A small miss preserves
the absolute timeline so the worker can catch up naturally. If the previous
deadline is already stale by at least the newly sampled interval, the software
backend rebases on the actual submission time; this prevents sustained overload
from turning all future randomized deadlines into already-expired timestamps.

## Backend classes

### `software-batch`

This is the portable default and the extraction target for the current worker
logic.  It waits against TSC, submits a complete burst with
`rte_eth_tx_burst()`, and records the deadline and submission time.  It supports
general NICs and microsecond/millisecond burst workloads, but does not control
intra-burst wire spacing.

### `hardware-rate`

This backend configures a queue shaper through a supported queue-rate or DPDK
Traffic Management API.  Software may fill the queue quickly while the NIC
scheduler controls its drain rate.  It is suitable for stable average rates and
traffic-class isolation.  It does not, by itself, express arbitrary Poisson or
Pareto per-packet deadlines, and its rate/burst granularity is device-specific.

Intel ice/E810 should first be evaluated through this class.  DDP primarily
changes parsing and classification and is not a scheduled-send interface.

### `hardware-scheduled`

This backend attaches an absolute NIC-clock deadline to individual mbufs and
uses a PMD/NIC scheduled-send facility.  It can represent fixed, Poisson, and
Pareto per-packet timing, but may split multi-packet WQEs, consume more
descriptors, increase PCIe traffic, and reduce maximum packet rate.  The backend
must submit sufficiently far ahead of the deadline and report packets that were
late, rejected, or scheduled too far into the future.

The first prototype targets mlx5/ConnectX hardware already represented in the
benchmark environment.  It is not enabled by default until wire-time accuracy
and throughput cost have both been measured.

## Capability model

Selection is based on runtime probing, not PCI IDs or driver-name assumptions.
At minimum a backend reports:

```c
enum pacing_scope { PACING_SCOPE_BURST, PACING_SCOPE_QUEUE,
                    PACING_SCOPE_PACKET };
enum pacing_clock { PACING_CLOCK_TSC, PACING_CLOCK_NIC,
                    PACING_CLOCK_PHC };

struct pacing_caps {
    enum pacing_scope scope;
    enum pacing_clock clock;
    uint64_t min_lead_time_ns;
    uint64_t max_future_time_ns;
    uint64_t timestamp_resolution_ns;
    uint32_t max_scheduled_packets;
    bool queue_rate_limit;
    bool per_packet_schedule;
    bool tx_timestamp;
};
```

Probe output, PMD, firmware, selected clock, and resolution are preserved with
the benchmark result.

## Backend contract

The backend interface owns capability probing, lifecycle, rate configuration,
submission, and optional timestamp collection:

```c
struct pacing_request {
    uint64_t target_ns;
    struct rte_mbuf **packets;
    uint16_t count;
};

struct pacing_result {
    uint16_t accepted;
    uint16_t sent_immediately;
    uint16_t late;
    uint16_t rejected;
    uint64_t submit_tsc;
};

struct pacing_backend_ops {
    int  (*probe)(uint16_t port, struct pacing_caps *caps);
    int  (*init)(struct pacing_ctx *ctx);
    int  (*configure_rate)(struct pacing_ctx *ctx,
                           uint64_t rate_bps, uint64_t burst_bytes);
    int  (*submit)(struct pacing_ctx *ctx,
                   const struct pacing_request *request,
                   struct pacing_result *result);
    int  (*read_tx_timestamps)(struct pacing_ctx *ctx, void *samples,
                               size_t sample_count);
    void (*destroy)(struct pacing_ctx *ctx);
};
```

The workload generator must not infer success from `accepted` alone.  Hardware
timestamps or xstats determine whether scheduled packets met their deadlines.

## Late policies

When a deadline is already in the past, configuration selects one explicit
policy:

| Policy | Behavior |
|--------|----------|
| `send-immediately` | Send now and increment late counters; rebase when lateness reaches one new interval; default |
| `drop-late` | Drop packets beyond the configured lateness budget |
| `reschedule` | Establish a new timeline from the current time |
| `catch-up` | Preserve the original timeline and submit until caught up |

Every policy publishes late count, maximum lateness, rejected count, and any
timeline resets.  Rigorous timing benchmarks should normally use `drop-late`;
functional traffic generation normally uses `send-immediately`.

## Configuration and fallback

```yaml
pacing:
  backend: auto                 # auto, software, hardware-rate, hardware-scheduled
  fallback: fail                # fail or software
  late-policy: send-immediately
  clock: nic
  max-lateness-ns: 1000
  schedule-ahead-us: 100
```

`software` always selects the portable backend.  Explicit hardware selection
fails when its required capability is absent unless `fallback: software` is
configured.  `auto` may choose hardware-rate for an average-rate workload and
hardware-scheduled for per-packet deadlines.  Benchmark mode must never
silently fall back: requested backend, selected backend, and fallback reason
are part of the result.

## Metrics and acceptance

All backends publish target, submission, late, rejected, and fallback metrics.
Hardware-scheduled backends additionally publish wire-error percentiles when a
valid egress-time source exists.  Histograms identify the clock and timestamp
source in their schema.

The mlx5 prototype is evaluated across batch size, queue count,
`schedule-ahead-us`, and target packet rate.  Acceptance records wire-error
p50/p99/max, MPPS, cycles per packet, WQE/descriptor pressure, late packets,
and rejected packets.  Nanosecond timing claims require independent wire-time
validation; high accuracy at an unusably low packet rate is not sufficient.

## Implementation sequence

Current status: the TX-only path is routed through a per-worker
`software-batch` context using absolute TSC deadlines, and early/late timing
histograms are merged on the statistics path.  mlx5 probing registers and
reports standard scheduled-timestamp mbuf metadata, but scheduling remains
disabled until NIC-clock calibration is implemented.

1. Add explicit backend selection, fallback and late-policy configuration.
2. Add NIC-clock calibration and enable the guarded mlx5 submission path.
3. Add explicit fallback, late policies, and persisted
   backend metadata.
4. Add `hardware-rate` and validate average rate and burst behavior.
5. Complete and benchmark the mlx5 `hardware-scheduled` submission path.
6. Measure wire accuracy and throughput cost before deciding any automatic
   selection policy.
7. Evaluate ice/E810 Traffic Management separately using the same contract.
