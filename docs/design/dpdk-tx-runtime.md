# DPDK TX runtime: first extraction

## Goal and non-goals

The shared runtime now contains common DPDK lifecycle, port/queue setup,
packet-pool ownership, and TX submission mechanics. Bless and Injector remain
responsible for YAML/CLI parsing, device classification, port/queue policy,
offload selection, packet construction, rate limiting, and worker policy.

This increment does not define RX packet processing, flow-rule, control-plane,
HTTP, telemetry, or protocol-generation APIs. The runtime API is DPDK-facing
and deliberately small; it is not yet a general plugin ABI.

## Ownership and lifecycle

The application passes the already-merged EAL argv to the runtime. The runtime
owns the call to `rte_eal_init()` and, after all lcores and ports have stopped,
the matching `rte_eal_cleanup()`. The application still owns ordering: workers
must be joined before port shutdown, and all ports must be stopped/closed before
runtime-owned packet pools are released.

Pools created through `br_runtime_pktmbuf_pool_create()` (including the
TX-specific convenience wrapper) are tracked in a thread-safe runtime registry.
The registry supports more than one pool per lcore, which is needed when the
main lcore creates a process-wide RX pool and later also owns a worker TX pool.
`br_runtime_mempools_release()` is valid only after workers stop and all ports
are closed. Applications retain control of when those lifecycle calls occur.

`br_runtime_port_configure()` accepts independent RX/TX queue counts and
descriptor counts, optional descriptor adjustment, final port/queue
configuration, and an optional RX pool. It configures queues but does not start
the device; applications can still apply settings such as ptype parsing before
calling `br_runtime_port_start()`. Device selection, final offload masks,
promiscuous mode, MAC reporting, and link wait remain application policy.

## Packet ownership contract

`br_runtime_tx_burst()` preserves DPDK `rte_eth_tx_burst()` semantics. On return
`n`, the PMD owns mbufs in `[0, n)`; the caller retains and must free mbufs in
`[n, count)`. The runtime does not retry, mutate, or free a burst. This contract
is required for correct TX completion and for pool shutdown ordering.

## Shared API surface

- initialize/clean up EAL;
- configure RX/TX queues independently, then start a port;
- stop and close enabled ports;
- create and track packet mbuf pools, releasing them after port shutdown;
- submit a TX burst with explicit ownership semantics.

## Acceptance gates

1. Injector builds and the finite PCAP schema smoke test passes.
2. The API remains in a separate static archive and can be linked independently
   of Injector application sources.
3. Both applications link the same runtime archive; Bless's default build,
   unit tests, and net_null TX smoke run pass while application-specific RX
   processing and port policy remain intact.
4. The DPDK net_ring runtime tests exercise generic RX/TX configuration,
   multi-pool registration, and shutdown. Physical-NIC validation must still
   verify graceful drain/close and mbuf pool release under traffic.

## Port and pool comparison

| Concern | Bless | Injector |
| --- | --- | --- |
| Queue direction | Physical/full-featured devices use RX and TX queues; simple vdevs use TX only | TX only |
| Queue count | `rxtxq_per_port` for full-featured devices; one TX queue for simple vdevs | One queue for simple vdevs; configured TX queue count otherwise |
| Descriptors | Adjusts RX and TX descriptor counts for full-featured devices | Adjusts TX descriptors for full-featured devices |
| Port configuration | Has device-specific RX/TX offloads, ptype parsing, promiscuous mode, MAC reporting, and link wait | Masks TX offloads to device capability and starts the port |
| Packet pools | One shared RX pool sized from RX rings, bursts, worker cache, plus worker-local TX pools | One worker-local TX pool per transmitting lcore |
| Shutdown | Joins workers, drains briefly, stops/closes selected ports, releases shared RX and worker TX pools | Joins workers, drains briefly, stops/closes selected ports, releases worker TX pools |

The generic configure/start split is now shared by both applications. The API
keeps mechanics separate from policy:

1. A generic port configuration structure provides independent RX/TX queue
   counts and descriptor counts, optional RX mempool, and explicit port/queue
   configuration. Selection, device classification, promiscuous mode, ptype
   policy, MAC reporting, and link-wait policy remain application-specific.
2. Configure/queue-setup and start are separate operations, preserving
   zero-RX TX-only operation for Injector and zero-RX simple-vdev operation for
   Bless.
3. Stop/close ordering is provided by the runtime helper; the caller provides
   the selected-port set and retains its existing drain delay.
4. The thread-safe pool registry now tracks both the shared RX pool and
   per-worker TX pools; both applications release registered pools after
   stopping/closing their ports.

Bless now explicitly releases its shared RX pool along with worker TX pools
after port close, resolving the previous omission in its shutdown path. This
cleanup relies on queues having been closed and workers having exited.

## Remaining validation

- Exercise full-featured RX/TX queue setup on the target physical NIC, including
  descriptor adjustment, RX pool attachment, offload configuration, and clean
  shutdown while traffic is active.
- Add focused runtime tests for invalid queue/pool configurations and partial
  queue-setup failures where the test environment can provide a suitable DPDK
  vdev or mock.
- Keep RX packet processing, flow rules, promiscuous mode, ptype policy, and
  link management in the applications until both have demonstrated a real
  shared requirement for those higher-level behaviors.
