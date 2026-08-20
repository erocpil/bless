# IPv6 Protocol Support: Design, Implementation, and Pitfalls

This document records the architectural decisions, implementation details, and two historical bug fixes encountered during the introduction of IPv6 packet construction capability into Bless.

---

## 1. Architectural Decision: Plugin Extension vs. Built-in Types

### Background

Bless's built-in types (ARP, IPv4/ICMP/TCP/UDP) are hardcoded in `include/cnode.h` as sub-structures of `ether.type`. QUIC was introduced using a **plugin extension** pattern (see `include/bless_plugin.h`), i.e., dynamically registering via `bless_register_pkt_type()` in an `__attribute__((constructor))` function.

### Decision

IPv6 follows the QUIC plugin pattern rather than being a built-in type.

**Trade-offs:**

| Dimension | Plugin Pattern | Built-in Type |
|------|---------|---------|
| Config Access | Via `cnode->ether.type.ipv6` direct field | Also direct |
| Build Registration | `constructor` function auto-registers | Compile-time hardcoding into function tables |
| YAML Parsing | Via the generic `bless_cfg` framework | Requires manual parsing code |
| Invasiveness | Low: 1 new file + `cnode.h` new field | Requires changes to multiple function tables |

Rationale:
- IPv6 code is self-contained in `src/proto_ipv6.c`, with changes affecting only `cnode.h` (config slot) and `config.c` (YAML parsing), leaving core dispatch logic untouched
- Consistent with the QUIC pattern, reducing cognitive overhead for future protocol extensions
- `constructor` functions run automatically at static link time with zero runtime overhead

### Files Involved

| File | Change |
|------|---------|
| `include/cnode.h` | Added `ether.type.ipv6` struct (address arrays + TCP/UDP ports) |
| `src/proto_ipv6.c` | Constructor `bless_mbufs_ipv6()` + mutation functions + plugin registration |
| `include/ipv6.h` | Mutation function declarations + `IPV6_MUTATORS_COUNT` |
| `include/erroneous.h` | Registered `{ "ipv6", ipv6_mutators, 4 }` |
| `src/config.c` | YAML `ether.type.ipv6` section parsing |
| `src/bless.c` | `bless_parse_ipv6_range()` |
| `include/bless.h` | `OFFLOAD_IPV6` macro + parse function declarations |
| `src/cnode.c` | `cnode_show()` IPv6 dump + `cnode_show_summary()` stats line |

---

## 2. Configuration Model

### YAML Structure

```yaml
bless:
  ether:
    type:
      ipv6:
        src: "2001:db8::1+100"
        dst: ["2001:db8::100", "2001:db8::200"]
        tcp:
          src: 10000+100
          dst: 80
        udp:
          src: 20000+100
          dst: [53, 443]
```

### Address Range Syntax

`bless_parse_ipv6_range()` supports the same syntax as IPv4: `address+N` means N+1 consecutive addresses starting from the base address. The design intentionally supports only a decimal integer after the `+` (it does not support colon-delimited IPv6 subnet increments, because `2001:db8::1+::10` would be ambiguous during parsing).

**Differences from IPv4 range:**

IPv4's `parse_ip_range()` uses `inet_pton(AF_INET)` to parse into a `uint32_t`, which naturally supports arithmetic increment. IPv6 addresses are 128-bit and do not support native C arithmetic. The `range` parameter serves only as a semantic label -- during actual packet transmission, the `RANDOM_IPV6_SRC/DST` macro randomly selects an index in the `[0, range]` interval. The index value only affects throughput dimension statistics and does not cover a contiguous address space.

### Port Configuration

Ports reuse IPv4's `parse_port_range()`, unchanged.

---

## 3. Packet Constructor

### Protocol Alternation

`bless_mbufs_ipv6()` alternates TCP/UDP based on `i & 1`, consistent with the QUIC constructor's TCP/UDP alternation logic.

- Prioritizes protocols with configured ports (if only TCP ports are configured, all traffic uses TCP)
- If neither port type is configured -> returns 0 (no packet output)

### HW Offload

```c
if (OFFLOAD_IPV6(cnode)) {
    m->ol_flags |= RTE_MBUF_F_TX_IPV6 | RTE_MBUF_F_TX_TCP_CKSUM;
    tcp->cksum = rte_ipv6_phdr_cksum(ipv6, m->ol_flags);
}
```

Enabled via `hw-offload: [ipv6, tcp, udp]`, consistent with IPv4's offload control pattern.

**DPDK 23.11 compatibility:** DPDK 23.11 removed the old `rte_ipv6_phdr_cksum()` macro, replacing it with `rte_ipv6_udptcp_cksum()` for direct pseudo-header checksum computation. The final solution calls `rte_ipv6_phdr_cksum()` directly (it is a DPDK inline wrapper that exists, but its header file path is `/opt/dpdk/include/rte_ip.h` rather than under `/usr/include/`).

---

## 4. Anomaly Injection Framework

### 4 Mutations

| Mutation | Behavior | Test Purpose |
|----------|------|---------|
| `mutation_ipv6_version` | Write version field as 4 (masquerading as IPv4) | Whether the gateway correctly rejects non-6 versions |
| `mutation_ipv6_traffic_class` | Write Traffic Class as 0xFF | DSCP/ECN parsing robustness |
| `mutation_ipv6_flow_label` | Randomly fill Flow Label | Multi-path load balancing verification |
| `mutation_ipv6_hop_limit` | Set Hop Limit to 0 | Whether the gateway triggers ICMPv6 Time Exceeded |

**Design choice:** Mutation functions directly operate on `rte_ipv6_hdr` inside the mbuf, located via `rte_pktmbuf_mtod_offset(m, struct rte_ipv6_hdr *, m->l2_len)`.

**Note:** Mutations are called **after** the constructor, at which point `m->l2_len` has been correctly set.

---

## 5. Bug Fix: `bless_set_dist` Call Ordering (P0)

### Symptoms

On first launch with `config-ipv6.yaml`, `bless_set_dist()` returned a `dist->mask` of NULL, causing SIGSEGV on dereference. Manifested as "segfault" without a clear error message.

### Root Cause

```
worker_init()
  +-- parse_and_merge_config()       <- entry point
        +-- parse_args(argc, argv)   <- parse CLI arguments
        |     +-- bless_set_dist()   <- [1] 1st call: YAML weights not yet loaded
        +-- config_parse()           <- parse YAML file
        |     +-- config_parse_bless()
        |           +-- parse dist: { ipv6: 100, ... }
        +-- [bless_set_dist not called] <- [2] missing!
```

`bless_set_dist()` was called during CLI parsing (handling only CLI arguments like `--ipv6=N`), but the `dist` weights in the YAML configuration were parsed later in `config_parse()`. YAML weights were never applied.

### Previous behavior

QUIC relied on CLI `--quic=100` as a workaround -- users manually specified weights on the command line. The YAML weight path was never actually exercised.

### Fix

```c
// Promote ratio/bep from local variables to file-level static
static uint16_t ratio16[MAX_TYPES];
static uint8_t  bep8[MAX_TYPES];

// Populate ratio16/bep8 inside config_parse()

// Move the call from parse_args() to the end of parse_and_merge_config()
parse_and_merge_config():
    parse_args()         // only parse CLI, do not call bless_set_dist
    config_parse()       // parse YAML -> populate ratio16/bep8
    bless_set_dist()     // [3] CLI and YAML weights now merged
```

Weight computation runs after all configuration sources have been merged. The
same ordering is required for any additional configuration source.

### Related Files

| File | Change |
|------|------|
| `src/config.c` | `ratio16`/`bep8` promoted to static; `bless_set_dist` moved to end of function |
| `src/main.c` | `parse_args()` split -- `bless_set_dist` moved to `parse_and_merge_config` |

---

## 6. `cnode_show()` IPv6 Dump Completion

### Motivation

The IPv6 config slot existed in `cnode.h` but had no output in `cnode_show()` -- during debugging, `--dump-config` only showed ARP and IPv4, while IPv6 configuration was silently skipped.

### Implementation

- Added `print_ipv6()` helper (`inet_ntop(AF_INET6, ...)`) outputting compressed format
- Inserted IPv6 block between ARP and IPv4 inside `cnode_show()`:
  address arrays (display up to ED=4 entries), TCP ports, UDP ports, and ranges
- Added IPv6 statistics line in `cnode_show_summary()`

Sample output:

```
ipv6: {
  n_src: 1, n_dst: 2
  src[1]: [ 2001:db8::1 ]
  dst[2]: [ 2001:db8::100, 2001:db8::200 ]
  tcp: {
    src[1]: [ 10000 ]
    dst[1]: [ 80 ]
    src_range: 100
    dst_range: 0
  }
  udp: {
    src[1]: [ 20000 ]
    dst[2]: [ 53, 443 ]
    src_range: 100
    dst_range: 0
  }
}
```

---

## 7. Pitfalls

### Pitfall 1: Symbol Lookup Path

`rte_ipv6_phdr_cksum` is defined in DPDK 23.11's `/opt/dpdk/include/rte_ip.h`, not under `/usr/include/`. An initial search with `grep -r rte_ipv6_phdr_cksum /usr/include/` returned no results, leading to the mistaken belief that pseudo-header checksum calculation had to be done manually.

**Lesson:** When DPDK is installed in a non-standard path, searches must include the actual installation directory (obtain `-I` paths via `pkg-config --cflags libdpdk`).

### Pitfall 2: `mutation.h` Duplicate Symbols

`proto_ipv6.c` directly `#include "mutation.h"`, causing non-static functions like `mutation_ipv4_frag` to be linked twice (`proto_quic.c` also included it).

**Fix:** Forward-declare `struct Mutator`, consistent with `proto_quic.c`.

### Pitfall 3: Implicit Assumptions After Cnode Size Expansion

IPv6's `uint8_t src[IP_ADDR_MAX][16]` (=1024*16=16KB per array) expands Cnode by approximately 32KB. `bless_init()` uses `malloc(sizeof(Cnode))` for allocation -> no issue. However, the `cnode_show()` dump function was not synchronously updated, so IPv6 configuration was invisible before the completion.

### Pitfall 4: Division by Zero from `n_src=0`

When the user omits the `src` configuration, the IPv6 parser leaves `n_src` as 0. `bless_mbufs_ipv6()`'s first line `if (!cnode->ether.type.ipv6.n_src || ...) return 0;` correctly guards against this, but in the YAML parsing path, `config_parse_ipv6()` had a `%0` risk in early versions -- the `n_src`/`n_dst` assignment logic was aligned with IPv4: when unspecified, set to `RANDOM_IPV6_SRC` (a single address) rather than 0.

---

## 8. Verification Results

| Metric | Value |
|------|-----|
| Build | `make -j` 0 errors 0 warnings |
| Unit tests | 1,154 parse/core tests + 27 IPv6 range tests passed |
| net_null smoke | 57M packets, 3.3 Mpps, zero crashes |
| `--dump-config` | IPv6 addresses/ports fully output |
| CI | Full pipeline passing, including `config-ipv6.yaml` smoke |

---

## 9. Related Documents

| Document | Content |
|------|------|
| `docs/concepts/entropy-theory.md` | Entropy injection theoretical foundations |
| `docs/entropy-control.md` | Weight distribution and protocol entropy control |
| `docs/design-decisions.md` | Architecture-level design decisions and trade-offs |
| `docs/reference/protocols/quic.md` | QUIC plugin pattern (IPv6 reference) |
| `docs/industry-showcase.md` | Industry showcase roadmap (including IPv6 progress) |
| `docs/guides/observability.md` | Observability dashboards and telemetry |
