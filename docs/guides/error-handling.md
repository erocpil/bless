# BLESS Error Handling Strategy

## Layered Contract

Errors in BLESS are classified into four severity levels, each with a defined
handling approach.

### L1: Full Process Exit (Fatal)

**Scope**: DPDK EAL initialization failure, config parsing failure, resource
allocation failure for resources shared by all workers.

**Handling**: `rte_exit(EXIT_FAILURE, "...")`, no recovery attempted. These errors
mean the system cannot start; continuing to run is meaningless.

```
init_eal()        <- EAL init failure -> rte_exit
init_config()     <- YAML unparseable  -> rte_exit
parse_args()      <- CLI args invalid  -> rte_exit
```

### L2: Single Worker Unavailable

**Scope**: Worker-specific resource allocation failure (mbuf pool, ring buffer,
handshake context).

**Handling**: That worker returns without entering the main loop. Does not affect
other workers or Main Lcore.

**Current status**: All fixed. Nine `rte_exit` calls in `worker.c` downgraded to
`LOG_ERR + graceful degradation`:

| Original Location | Function | Handling |
|-------------------|----------|----------|
| `worker.c:232` | `worker_func_tx_only` | `LOG_ERR + return -1` |
| `worker.c:241` | `worker_func_tx_only` | `LOG_ERR + return -1` |
| `worker.c:402` | `worker_func_rx_only` | `LOG_ERR + return -1` |
| `worker.c:488` | `worker_func_fwd` | `LOG_ERR + return -1` |
| `worker.c:718` | `worker_func_handshake` | `LOG_ERR + return -1` |
| `worker.c:735` | `worker_func_handshake` | `LOG_ERR + return -1` |
| `worker.c:1027` | `worker_loop` | `LOG_ERR + barrier + idle-until-EXIT` |
| `worker.c:1082` | `worker_loop` | `goto INIT_FAIL + barrier + idle` |
| `worker.c:1100` | `worker_loop` | `goto INIT_FAIL + barrier + idle` |

### L3: Single Packet Construction Failure

**Scope**: `bless_mbufs()` construction error, `mutate()` mutation failure, mbuf
allocation exhaustion.

**Handling**: Skip the packet, LOG_WARN to record the count, continue to the next
batch. **Do not kill the process**.

```
worker_tx_loop
  +-- bless_mbufs() -> returns 0  -> log + skip, continue
  +-- mutate()      -> returns 0  -> log + skip, continue
  +-- tx_burst()    -> returns < n -> retry or drop
```

### L4: Web Request Failure

**Scope**: Errors inside HTTP handlers, WebSocket write failure, JSON serialization
failure.

**Handling**: Return HTTP 500 or silently drop. Does not propagate to the DPDK
data plane.

---

## Current Status Cross-Reference

| Location | Severity Level | Current Handling | Issue |
|----------|---------------|-----------------|-------|
| `config.c:1342` | L1 | `rte_exit` | OK |
| `worker.c:232` | L2 -> L1 | `rte_exit` | **Should be L2**, should not kill other workers |
| `worker.c:241` | L2 -> L1 | `rte_exit` | Same as above |
| `worker.c:301` | L3 -> L1 | `rte_exit` | **Should be L3**, single-packet failure should not kill process |
| `worker.c:322` | L3 -> L1 | `rte_exit` | Same as above |
| `worker.c:401` | L2 -> L1 | `rte_exit` | Should be L2 |
| `worker.c:487` | L2 -> L1 | `rte_exit` | Should be L2 |

---

## Function Error Return Contract

| Function | Return Type | Success | Failure | Caller Handling |
|----------|------------|---------|---------|-----------------|
| `bless_mbufs()` | int | positive | 0 | Skip packet, count |
| `bless_alloc_mbufs()` | int | 0 | -1 | Skip batch |
| `mutate()` (per-mutation functions) | int | positive | 0 | Skip mutation |
| `parse_args()` | int | 0 | -1 | rte_exit (L1) |
| `config_init()` | Node* | non-NULL | NULL | rte_exit (L1) |
| `rte_eth_tx_burst()` | uint16 | >0 | 0 | Retry |

---

## Guidelines

1. **Do not use `rte_exit` on the data-plane path**. The worker main loop is the
   data-plane path, running on every nanosecond. The cost of killing the process is
   far higher than dropping a single packet.
2. **`LOG_ERR` is more appropriate than `rte_exit`**. Most paths that need to
   terminate should use `LOG_ERR` first, letting the caller decide whether to continue.
3. **Worker startup failure should not affect already-started workers**. Resource
   allocation failures during startup need only mark that worker as unavailable;
   Main Lcore continues to run.
