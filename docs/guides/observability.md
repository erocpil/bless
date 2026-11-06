# Observability

Bless exposes real-time telemetry via three channels: Prometheus metrics, a
WebSocket push stream, and an embedded HTML dashboard.  This document covers
setup and usage of the Prometheus + Grafana path.

---

## Prometheus `/metrics`

The server currently starts with Bless and does not yet have a
`server.enable` switch.

Configure the listener under `system.server.options`, and keep host deployments
bound to loopback until the control and observation endpoints can be enabled
independently.

The endpoint is at `http://<host>:8000/metrics`.

### Throughput

| Metric | Type | Description |
|--------|------|-------------|
| `bless_tx_mpps` | Gauge | TX million packets per second |
| `bless_rx_mpps` | Gauge | RX million packets per second |
| `bless_tx_gbps` | Gauge | TX gigabit per second |
| `bless_rx_gbps` | Gauge | RX gigabit per second |
| `bless_rx_loss_rate` | Gauge | RX packet loss ratio (0-1) |

### Shannon Entropy (15 dimensions)

`bless_entropy{type="<dim>"}` -- Gauge, value in bits (0 to log2 N).

| `type` label | Dimension |
|--------------|-----------|
| `protocol` | Protocol type distribution |
| `src_ip` | Source IP address entropy |
| `dst_ip` | Destination IP address entropy |
| `src_port` | Source port entropy |
| `dst_port` | Destination port entropy |
| `pkt_size` | Packet size distribution |
| `tcp_flags` | TCP flag combinations |
| `total_5tuple` | Joint (proto, sip, dip, sport, dport) |
| `joint_5tuple` | Full 5-tuple joint entropy |
| `delta_tsc` | Inter-packet timing entropy |
| `vxlan_encap` | VXLAN encapsulation presence |
| `outer_src_ip` | VXLAN outer source IP |
| `outer_dst_ip` | VXLAN outer destination IP |
| `outer_src_port` | VXLAN outer UDP source port |
| `vni` | VXLAN Network Identifier |

### Min-Entropy (H-infinity)

`bless_entropy_min{type="<dim>"}` -- Gauge.  Measures the worst-case single-value
replay rate.  Low min-entropy means one value dominates (poor diversity).

### Max Entropy

`bless_entropy_max{type="<dim>"}` -- Gauge.  The theoretical ceiling for each
dimension (log2 of the configured value range).

### Latency

`bless_lat_p50_us`, `bless_lat_p95_us`, `bless_lat_p99_us`, `bless_lat_p999_us` --
Gauge, microseconds.  14-bucket log-scale histogram (0-10000 us).
`bless_lat_samples` -- total samples in the current window.

Requires `injector.latency-hist-enable: true` and a return path. In `handshake`
mode the sample is the SYN-to-SYN-ACK RTT; in receive/forward modes it is
derived from the packet timestamp supported by that worker. See the
[dual-endpoint handshake experiment](handshake-experiment.md) for a complete
functional setup.

### DPDK Port Stats

| Metric | Type | Labels |
|--------|------|--------|
| `dpdk_ipackets` | Counter | `port` |
| `dpdk_opackets` | Counter | `port` |
| `dpdk_ibytes` | Counter | `port` |
| `dpdk_obytes` | Counter | `port` |
| `dpdk_imissed` | Counter | `port` |
| `dpdk_ierrors` | Counter | `port` |
| `dpdk_oerrors` | Counter | `port` |

### System

| Metric | Description |
|--------|-------------|
| `bless_process_cpu_cores` | Process CPU seconds consumed per wall-clock second; `1.0` means one CPU core of aggregate process time |
| `bless_enabled_lcores` | Enabled DPDK lcores used as the normalization denominator |
| `bless_enabled_lcore_utilization_ratio` | `process_cpu_cores / enabled_lcores`; may exceed `1.0` when non-EAL threads add CPU time |
| `bless_cpu_busy_pct` | Deprecated compatibility metric: `min(enabled_lcore_utilization_ratio * 100, 100)` |
| `bless_mem_rss_kb` | Process RSS in KB |
| `bless_cpu_frequency_khz{type="min|max"}` | Enabled-lcore frequency range sampled by the control path |
| `bless_context_switches_total{type="voluntary|involuntary"}` | Cumulative process context switches |
| `bless_tx_submit_overshoot_us` | Delay between a software pacing deadline and entry to the PMD submit call |
| `bless_tx_submit_overshoot_max_us` | Largest deadline overshoot in the current window |
| `bless_tx_burst_duration_us` | Time spent in `rte_eth_tx_burst()` |
| `bless_tx_burst_duration_max_us` | Longest PMD submit call in the current window |
| `bless_tx_build_cycles_per_pkt` | TSC cycles per packet for construction (`bless_mbufs` + mutation + shuffle) in the current window |
| `bless_tx_submit_cycles_per_pkt` | TSC cycles per packet inside `rte_eth_tx_burst()` in the current window |
| `bless_tx_cycles_per_pkt` | Real send cost per packet = build + submit (excludes pacing busy-wait) |
| `bless_tx_wait_ratio` | Pacing busy-wait as a fraction of build + wait + submit (0.0–1.0); the complement is actual send work |
| `bless_sampler_samples` | Entropy samples used by the current calculation |
| `bless_sampler_overwritten_total` | Cumulative unread entropy samples replaced in the ring |
| `bless_sampler_overwritten_window` | Entropy samples replaced since the previous calculation |

### Sampling quality

The entropy sampler selects packets using a per-worker randomized sequence and
commits a sample only after the TX device accepts that packet. This prevents a
fixed sample interval from locking onto batch or protocol cycles, and prevents
partial TX bursts from counting packets that never left Bless.

`bless_sampler_samples` is the number of selected samples used in the current
calculation. It is not a packet counter. `bless_sampler_overwritten_window`
reports selected samples that were replaced before the observer read them. A
non-zero value means the entropy window is incomplete and should not be used as
an unbiased description of all transmitted traffic. Increase
`sample-interval`, shorten the observation interval, or move observation work
to a less busy core before interpreting that window.

The current implementation uses a fixed per-worker sample buffer, not an
`rte_ring`. Hardware mirroring can be useful as an independent calibration
source, but HyperLogLog only estimates cardinality and Count-Min Sketch adds
collision error; neither is a drop-in replacement for the joint distributions
needed by entropy and mutual-information calculations.
| `bless_entropy_min_target{type,source}` | Configuration-aware or observed-support min-entropy baseline |
| `bless_entropy_min_gap_bits{type}` | Non-negative target minus measured min-entropy |
| `bless_entropy_min_attainment_ratio{type}` | Fraction of the target attained, capped at 1 |
| `bless_entropy_min_dominance_ratio{type}` | Excess dominant-value concentration (`2^gap_bits`) |
| `bless_entropy_min_samples{type}` / `bless_entropy_min_distinct{type}` | Evidence population for the diagnosis |
| `bless_entropy_min_state{type,state}` | One-hot diagnostic state, including inactive and insufficient samples |

### Handshake (handshake mode only)

| Metric | Description |
|--------|-------------|
| `bless_hs_cps` | Connections per second |
| `bless_hs_success_rate` | ESTABLISHED / SYNs sent |
| `bless_hs_syn_sent_total` | Cumulative SYNs sent |
| `bless_hs_syn_recv_total` | Cumulative SYNs received |
| `bless_hs_synack_sent_total` | Cumulative SYN-ACKs sent |
| `bless_hs_synack_recv_total` | Cumulative SYN-ACKs received |
| `bless_hs_ack_sent_total` | Cumulative ACKs sent |
| `bless_hs_established_total` | Total established connections |
| `bless_hs_rst_sent_total` | RSTs sent (timeouts, rejections) |
| `bless_hs_rst_recv_total` | RSTs received |
| `bless_hs_timed_out_total` | Connections that timed out |
| `bless_hs_conn_current` | Currently active connections |
| `bless_hs_conn_max` | Peak concurrent connections |

---

## Prometheus Configuration

Add to `prometheus.yml`:

```yaml
scrape_configs:
  - job_name: 'bless'
    scrape_interval: 1s
    static_configs:
      - targets: ['<bless-host>:8000']
```

The 1-second scrape interval matches Bless's 1-second stats snapshot cycle.

---

## Grafana Dashboard

A pre-built dashboard JSON is at `tools/grafana/bless-dashboard.json`.

### Import steps

1. Grafana -> Dashboards -> Import -> Upload JSON file
2. Select your Prometheus datasource
3. The dashboard has these sections:
   - **Throughput** -- TX/RX PPS + Gbps + loss rate + CPU/memory
   - **Shannon Entropy** -- 14-dimension time series
   - **Min-Entropy** -- H(infinity) per dimension
   - **Latency** -- p50/p95/p99/p99.9 percentiles
   - **DPDK Port Stats** -- packet rate, byte rate, errors, missed
   - **Handshake** -- collapsed by default; TCP 3-way state machine

### Panel descriptions

- **Entropy panels**: the y-axis is bits.  15 dimensions on one chart show
  which dimensions are saturated and which are starved.  A good configuration
  should push all lines toward their `bless_entropy_max` ceiling.
- **Min-entropy panels**: watch for drops -- a dimension with high Shannon
  entropy but low min-entropy has one value dominating (e.g., a single source
  port repeated 50% of the time).
- **Latency panel**: requires `latency-hist-enable: true` and a loopback
  through the DUT.  p99.9 tail is the key metric for jitter-sensitive gateways.
- **DPDK port stats**: `dpdk_imissed` and `dpdk_oerrors` should stay at zero.
  Non-zero values indicate queue overflow or hardware errors.

---

## WebSocket Push

The WebSocket endpoint at `ws://<host>:8000/wsURL` pushes stats JSON at 3 Hz.
Each frame is a JSON object with the same fields as the `/metrics` endpoint plus
the 12-pair MI matrix.

Useful for custom tooling, real-time dashboards, or CI pipelines that need
sub-second stats polling.

When `BLESS_API_KEY` is enabled, append it to the WebSocket URL:

```text
ws://127.0.0.1:8000/wsURL?api_key=<BLESS_API_KEY>
```

In `tools/www/index.html`, enter the endpoint in Server and the credential in
the separate API Key field. Use an absolute URL when the page and Bless are
exposed on different ports. Entropy and Observe are enabled only after that
connection succeeds and use its verified origin. The key is transferred to
those pages in a URL fragment that is immediately removed, and is attached as
a query parameter only to the WebSocket upgrade request.

---

## Structured Logging

`--log-format=json` produces one JSON object per log line:

```json
{"ts":"14:32:01.234","lvl":"INFO","tid":12345,"cpu":2,"msg":"core 2 exit normally"}
```

systemd's journald auto-parses JSON fields when `StandardOutput=journal`.
The systemd unit in `tools/bless.service` enables this by default.
