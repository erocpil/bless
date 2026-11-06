# Runtime Config Concurrency Model

This document describes how runtime configuration updates interact with
worker-core packet transmission and the C11 memory-ordering guarantees.

## Data Model

```
                    +-----------+
            +------>| Worker 0  |----+
            |       +-----------+    |
            |       +-----------+    |
   WS (set)+---+--->| Worker N  |----+---> rte_eth_tx_burst
            |   |   +-----------+    |
            |   |                    |
   base->bconf  |    (shared, one instance)
   (bless_conf)  |
                 +--- plain fields (display/logging/init)
                 +--- runtime_control
                        +--- mutex (plain control fields)
                        +--- _Atomic shadows (hot-path worker reads)
```

There is exactly **one** `bless_conf` instance (`base->bconf`), allocated
at process init and shared by all worker lcores.  Each worker also holds a
`worker.conf` struct copy taken at init via `memcpy(conf, bconf, offsetof(…,
dist_ratio))`, but that copy is **never used for live-updated parameters**.

Workers read live-updated fields through `runtime_control_load_*()` accessors.
The `runtime_control` object is placed after `dist_ratio` in `bless_conf` to
stay outside the worker-init `memcpy` range.

## Field Groups

### Group A: _Atomic hot-path fields (C11 data-race-free)

| Field | Type | Read by | Written by | Ordering |
|-------|------|---------|------------|----------|
| `runtime.pps_rate` | `_Atomic uint32_t` | workers (PPS sync loop), entropy-adaptive (control thread) | WS set (via apply callback), entropy-adaptive (control thread) | relaxed |
| `runtime.entropy_target` | `_Atomic double` | main lcore (adaptive rate) | WS set (via apply callback) | relaxed |
| `runtime.entropy_dim` | `_Atomic uint8_t` | main lcore (adaptive rate) | WS set (via apply callback) | relaxed |
| `runtime.entropy_adapt_gain` | `_Atomic double` | main lcore (adaptive rate) | WS set (via apply callback) | relaxed |
| `sampler.sample_interval` | `_Atomic uint32_t` | workers (sampling selection) | WS set (via apply callback) | relaxed |
| `g_master_seed` | `_Atomic uint64_t` | workers (start/reseed) | WS seed set | relaxed |

Concurrent state is accessed through C11 atomic load, store, or
compare-exchange operations with `memory_order_relaxed`.  Plain mirrors
used by set/get are protected by `runtime.mutex`.

`runtime.pps_rate` is the sole concurrent state for the PPS rate value:
- The **entropy-adaptive control loop** (running on the main thread) atomically loads
  `runtime.pps_rate`, computes an adjustment, and publishes it with a
  single-attempt
  compare-exchange.  If a concurrent explicit WS update changed the value, the
  compare-exchange fails and the user's value wins.
- The **WS set handler** writes the plain `pps_rate` field under
  `runtime.mutex`, then its per-field apply callback publishes only that field
  to `runtime.pps_rate`. Updating an unrelated runtime field cannot reset the
  adaptive PPS value.

All concurrent accesses use `memory_order_relaxed` because no ordering
dependency exists between these independently-updated fields.

### Group B: Startup-only fields (no data race)

`batch`, `num`, `traffic_model`, `batch_delay_us`, `batch_jitter_us`,
`pps_burst`, `bps_rate`, `bps_burst`, `hs_rate`, `hs_timeout_us`,
`hs_mix_ratio` — written once at init, read only at worker startup
(via `memcpy` into `worker.conf` or as one-shot init parameters for
token-bucket construction).  Marked `FIELD_STARTUP` in the runtime field
descriptor table; the WS set handler rejects runtime writes.

### Group C: Plain display/logging fields (mutex-protected)

`pps_rate` (plain), `sample_interval` (plain, in `bless_conf` only),
entropy_dim (plain), `entropy_target` (plain), `entropy_adapt_gain` (plain).

These are plain (non-atomic) mirrors read and written by CivetWeb worker
threads under `runtime.mutex`.
The actual concurrent state is in the corresponding `_Atomic` shadow
(Group A).  A control-plane thread that reads the plain field may see a
value that is stale relative to the atomic shadow, but this is only used
for display.

## Write Side: WebSocket Handler

The `ws_user_func` callback processes `{"cmd":"set", ...}` through the
runtime field descriptor table (see `src/runtime_field.c`):

1. Look up the field name in `runtime_fields[]`
2. Validate the JSON value (type, range, NaN/Inf rejection)
3. Reject startup-only fields
4. Lock `runtime.mutex`, write the plain `bconf` field, and invoke that field's
   apply callback
5. The callback publishes only the changed field through the corresponding
   `runtime_control_publish_*()` accessor
6. Unlock `runtime.mutex`

`bless_sync_atomic_runtime()` initializes all shadow fields before worker
startup; runtime updates use per-field callbacks to avoid lost updates.

## Read Side: Worker Cores

Workers read PPS periodically through its atomic shadow; the main lcore reads
the entropy-control fields for adaptation:

```c
/* pps_rate: read once per second, relaxed */
uint32_t live_pps =
    runtime_control_load_pps_rate(&conf->base->bconf->runtime);
```

```c
/* main lcore: entropy target/dim/gain per statistics update */
double target = runtime_control_load_entropy_target(&bconf->runtime);
uint8_t dim = runtime_control_load_entropy_dim(&bconf->runtime);
double gain = runtime_control_load_entropy_adapt_gain(&bconf->runtime);
```

`memory_order_relaxed` compiles to the same machine code as a plain load
on x86-64 — zero overhead.  It provides C11 data-race freedom and silences
ThreadSanitizer, which is the primary motivation.

## What Is NOT Guaranteed

### No Multi-Field Atomicity

Changing two parameters in separate `set` messages does **not** provide a
snapshot boundary.  One worker may pick up `pps_rate = 5000` while another
worker is still reading the old value.  Within a single worker, two fields
may update on different loop iterations.

For coupled updates, see the [generation-boundary design](../design/generation-boundary.md).
A counter around concurrent plain-field reads/writes would not remove C11 data races.

### No Causal Read-Write Ordering

A worker that reads `runtime.pps_rate = 0` (disabled) may still be operating on
a batch that was constructed before `pps_rate` was changed to zero.  The
old batch drains naturally through the token bucket in at most one loop
iteration.

## Stats vs. Config: Why Two Models

| Property | Stats (guarded double buffer) | Config (_Atomic shadows) |
|----------|----------------------|--------------------------|
| Access pattern | One writer, many readers | Multiple control writers, many readers |
| Consistency need | Atomic snapshot of struct | Per-field, best-effort |
| Update frequency | Configured statistics interval; broadcast independently at ~3 Hz | Explicit requests and periodic adaptation |
| Mechanism | Active index plus per-slot pin/retry reader counts | `atomic_store` per field (relaxed) |

## Automated coverage

CI links the production `runtime_control` and `stats_guard` modules into
a DPDK-independent multi-reader/multi-writer ThreadSanitizer stress test.
This exercises those modules, not every DPDK worker/control-plane interaction.
See [testing coverage](testing-coverage.md) for integration boundaries.

## Future Work

Multi-field commands must define which readers need a coherent snapshot.
A mutex can serialize control-plane transactions; lock-free readers need an
appropriate atomic representation and a reader-lifetime protocol.
See the [multi-field update design](../design/generation-boundary.md); it is a
proposal, not an implemented batch-set API.
