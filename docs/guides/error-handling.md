# BLESS Error Handling Strategy

Errors have four policy levels.
The descriptions below identify current functions rather than historical line
numbers; they do not imply exhaustive failure-injection coverage.

## Layered Contract

### L1: Process startup failure

DPDK EAL/device initialization, invalid startup configuration and shared-resource
allocation can prevent the process from starting.
`main.c` owns this policy and may terminate through `rte_exit()`.
The configuration modules return errors instead of terminating the process:
`config_init()` in `config_file.c` returns `NULL`, and configuration/domain
parsers and clone functions return failure to their callers.
See [Configuration code layout](../reference/config-architecture.md).

### L2: Worker unavailable

`worker_loop()` in `src/worker.c` handles worker allocation and initialization
failures without intentionally terminating other workers.
Failures before the startup barrier still join that barrier, then wait for
`STATE_EXIT`, so another lcore is not stranded waiting for initialization.
Mode functions such as `worker_func_tx_only()` return `-1` for their own setup
failures after the common initialization stage.
This documents the intended isolation boundary, not a guarantee of complete
cleanup for every allocation failure; automated OOM injection is still absent.

### L3: Packet construction or submission failure

In `worker_func_tx_only()`:

- `bless_mbufs()` returning zero causes that mbuf to be freed and removed from
  the contiguous burst; construction proceeds with the remaining packets.
- A mutation callback returning zero is logged; the worker does not terminate
  the process or automatically discard the packet for that return alone.
- `pacing_submit()` returns the count accepted by `rte_eth_tx_burst()`.
  The worker frees constructed packets that were not sent and consumes tokens
  only for accepted packets/bytes. It does not promise an unlimited retry loop.

A PMD accepting a packet is not proof that the packet reached the wire or peer.
Other worker modes have their own RX/TX behavior; do not generalize the TX-only
partial-burst policy into a universal retry contract.

### L4: Control-plane request failure

Frame validation, JSON command errors and socket write failures belong to the
control-plane request or connection.
HTTP and WebSocket have different reply/error paths; there is no universal
"HTTP 500" return for all failures.
See [API error codes](../reference/api.md#error-codes) and
[concurrency guarantees](../reference/concurrency.md).

## Function Error Return Contract

| Function | Return contract | Caller policy |
|---|---|---|
| `bless_mbufs()` (`src/bless.c`) | `uint64_t`: constructed byte count, or zero on failure | TX-only frees failed packet and continues |
| `bless_alloc_mbufs()` (`src/bless.c`) | `int`: requested count `n` on success, `-1` on failure | Worker setup reports failure |
| `bless_create_pktmbuf_pool()` (`src/bless.c`) | Pool pointer, or `NULL` on failure | Caller must handle allocation failure |
| `config_init()` (`src/config_file.c`) | `Node *`, or `NULL` on failure | Startup caller decides whether to exit |
| `config_clone_cnode()` (`src/config.c`) | Status; clone published only after successful construction | Worker initialization rejects a failed clone |
| `rte_eth_tx_burst()` | Accepted packet count, possibly less than requested | Ownership of unsent packets remains with caller |

## Maintenance and verification

When adding an error path, identify the owner of every allocation and the
startup barrier participation required before returning.
Do not add process termination to reusable configuration or packet-construction
helpers merely because their current caller runs during startup.
Check the linked production modules in the tests, rather than using a historical
"all fixed" list as evidence.
[Testing Coverage Boundaries](../reference/testing-coverage.md) records the
remaining OOM, shutdown and live integration limits.
