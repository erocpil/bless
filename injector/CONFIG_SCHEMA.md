# Injector configuration contract

Injector follows the same top-level layout as Bless:

```yaml
dpdk:       # DPDK EAL options
injector:   # TX-only worker and traffic-generation options
bless:      # packet description
system:     # process-level options
```

The former `application:` packet-description root is not supported. Configs
must use `bless:`. `make test-injector` checks both this migration and a
32-packet PCAP send.

## Supported subset

| Section | Injector behavior |
| --- | --- |
| `dpdk` | Scalar and sequence values are passed to DPDK EAL. Availability depends on the installed DPDK build and PMDs. |
| `injector` | Supports `p`, `q`, `num`, `batch`, `batch-delay-us`, `batch-jitter-us`, `traffic-model`, `bench-mode`, `weight`, `arp`, `icmp`, `tcp`, `udp`, `pps-rate`, `pps-burst`, `bps-rate`, `bps-burst`, `interleave`, `interleave-depth`, `seed`, and `log-format`. |
| `injector.mode` | May be omitted or set to `tx-only`; other modes are rejected. |
| `injector.auto-start` | May be omitted or set to `true`; Injector always starts its TX workers. `false` is rejected. |
| `bless` | Supports Ethernet/ARP/IPv4/ICMP/TCP/UDP packet descriptions, VXLAN, supported checksum offloads, and supported erroneous-packet mutations. Unsupported protocols are rejected by config validation. |
| `system` | Supports `daemonize`. Enabling `system.server` is rejected because Injector has no HTTP/WS server. Other UI-only settings are not consumed. |

Unknown keys in the `injector` section are rejected before EAL startup rather
than being silently passed as CLI flags. Bless-only injector options such as
promiscuous mode, forwarding port maps, periodic statistics, and entropy
sampling are not part of Injector's supported subset. Keep those options out
of Injector configs; use only values it implements.

The packet-description parser also rejects `proto`, `src-range`, and
`dst-range` fields, as well as the listed unsupported protocols. The example
configuration omits these fields rather than showing ignored settings.

This document describes schema compatibility, not a shared parser or shared
runtime API. The two applications still parse their configuration separately.
