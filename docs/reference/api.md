# BLESS HTTP / WebSocket API

## Basic Information

| Item | Value |
|------|-------|
| Default port | 8000 (HTTP) |
| HTTPS port | 8443 (requires `conf/ssl/ssl.pem`) |
| WebSocket endpoint | `/wsURL` |
| WS broadcast frequency | 3 Hz (every 333 ms) |
| WS subprotocols | None |

---

## Server Configuration

The embedded civetweb HTTP/WebSocket server is configured via YAML:

```yaml
system:
  server:
    enable: true
    control_enable: false
    remote_control_enable: false
    options:
      listening_ports: "127.0.0.1:8000"
      document_root: /tmp
      error_log_file: /tmp/bless-server-err.log
```

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `listening_ports` | string | `127.0.0.1:8000` | Bind address (use `127.0.0.1` for local-only, `0.0.0.0` for remote access) |
| `num_threads` | string | `4` | CivetWeb worker thread count |
| `enable_keep_alive` | string | `yes` | CivetWeb keep-alive setting |
| `request_timeout_ms` | string | `2000` | Request timeout in milliseconds |
| `document_root` | string | `/tmp` | Directory for static file serving |
| `access_log_file` | string | `/tmp/access.log` | Server access log path |
| `error_log_file` | string | `/tmp/error.log` | Server error log path |
| `ssl_certificate` | string | empty | PEM file used by a TLS listener |

### Control-plane security switches

These YAML keys live under `system.server` (not `system.server.options`):

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `enable` | bool | `true` | If `false`, the HTTP/WS server is not started |
| `control_enable` | bool | `false` | If `true`, WebSocket control commands (start/stop/set) are accepted |
| `remote_control_enable` | bool | `false` | If `true`, non-loopback listeners are allowed; requires `BLESS_API_KEY` |

When `control_enable` is `false`, only observation endpoints
(`/api/stats`, `/metrics`, `/entropy`, `/observe`) are served — the
WebSocket control plane is not registered.  Remote control
(`remote_control_enable: true`) must be paired with a valid
`BLESS_API_KEY` environment variable; the server refuses to start
otherwise.

### HTTPS

Configure a TLS listener such as `8443s` in `listening_ports` and set
`ssl_certificate` to a PEM file.

Self-signed certificate generation:

```bash
openssl req -x509 -newkey rsa:4096 -keyout conf/ssl/ssl.pem \
  -out conf/ssl/ssl.pem -days 365 -nodes -subj "/CN=localhost"
```

### Security Note

The built-in default is `127.0.0.1:8000` (loopback-only).

Non-loopback listeners (e.g. `8000`, `0.0.0.0:8000`) require setting
`remote_control_enable: true` and `BLESS_API_KEY` — the server
refuses to start otherwise.

WebSocket authentication is enabled by setting `BLESS_API_KEY` to a value from
16 through 255 characters. A strong value can be generated with
`export BLESS_API_KEY="$(openssl rand -hex 32)"`.

Clients then provide the same value in the WebSocket upgrade request's
`api_key` query parameter.

For `tools/www/index.html`, enter the endpoint and credential in the separate
Server and API Key fields:

```text
Server:  ws://127.0.0.1:8000/wsURL
API Key: <BLESS_API_KEY>
```

If the page is served from another port, use this complete URL rather than a
relative `/wsURL`, which resolves against the page origin. For a same-origin
page, the legacy `/?api_key=<BLESS_API_KEY>` form also populates the API Key
field and is immediately removed from the address bar. The page persists only
the Server URL, never the credential. Entropy and Observe remain disabled until
the WebSocket connection succeeds, then use that verified server origin. The
in-memory key is transferred in a URL fragment and immediately removed by the
new page; fragments are not sent in HTTP requests.

Authentication remains disabled when the environment variable is absent,
but only loopback listeners are allowed in that mode.

---

## Static Pages

### `GET /`
Web dashboard home page served from the configured document root.

### `GET /entropy`
Entropy panel. Embedded HTML + Chart.js, displays Shannon H, Min-entropy, Mutual
Information, and other metrics in real time. Receives 3 Hz updates via WebSocket.

### `GET /observe`
Real-time traffic observation panel. Embedded HTML showing port statistics, rate
curves, and packet construction distribution.

### `GET /assets/chart.min.js`
Locally hosted Chart.js library (~500 KB), no external CDN dependency.

---

## HTTP JSON API

### `GET /api/stats`
Full runtime statistics in JSON format.

**Response example:**

```json
{
  "meta": {
    "timestamp_ns": 1720519680123456789,
    "source": "Bless Injector",
    "schema_version": 7
  },
  "effective_config": {
    "batch": 64,
    "traffic_model": 1,
    "batch_delay_us": 1000,
    "batch_jitter_us": 0,
    "sample_interval": 10
  },
  "ports": {
    "0": {
      "stats": {
        "ipackets": 0,
        "opackets": 450000000,
        "ibytes": 0,
        "obytes": 28800000000,
        "imissed": 0,
        "ierrors": 0,
        "oerrors": 0,
        "rx_nombuf": 0
      },
      "xstats": {
        "rx_good_packets": 0,
        "tx_good_packets": 450000000
      }
    }
  },
  "entropy": {
    "protocol": 1.23,
    "src_ip": 7.83,
    "dst_ip": 3.21,
    "pkt_size": 2.45,
    "joint_5tuple": 11.24,
    "mi_sip_dip": 0.012,
    "sampler_samples": 4096,
    "sampler_overwritten": 12000,
    "sampler_overwritten_window": 8000
  },
  "psd": {
    "dominant_hz": 1992.1875,
    "strongest_peak_hz": 1992.1875,
    "fundamental_hz": 1015.625,
    "spectral_flatness": 0.87,
    "signal_valid": true,
    "mean_ppms": 64.0,
    "variation_rms_ppms": 120.0,
    "bins": [0.0, 0.12, 0.08]
  },
  "observe": {
    "rx_mpps": 0.0,
    "tx_mpps": 1.25,
    "rx_gbps": 0.0,
    "tx_gbps": 0.8,
    "rx_loss_rate": 0.0,
    "process_cpu_cores": 2.865,
    "enabled_lcores": 5,
    "enabled_lcore_utilization_ratio": 0.573,
    "cpu_busy_pct": 57.3,
    "tx_build_cycles_per_pkt": 450.0,
    "tx_submit_cycles_per_pkt": 5200.0,
    "tx_cycles_per_pkt": 5650.0,
    "tx_wait_ratio": 0.85,
    "mem_rss_kb": 1567892,
    "tx_submit_overshoot_p99_us": 3.0,
    "tx_burst_duration_p99_us": 18.0,
    "lat_p99_us": 12.0
  },
  "log": {
    "text": "log: 123456789"
  }
}
```

`effective_config` is published by the worker path. Unlike the WebSocket
control-plane response, these values are the settings currently used to build
and pace traffic.

### `GET /api/config`

Returns only the worker-effective configuration. Use this endpoint for health
checks that do not need the full statistics payload.

```json
{"effective_config":{"batch":64,"traffic_model":1,"batch_delay_us":1000,"batch_jitter_us":0,"sample_interval":10}}
```

| Top-level field | Type | Description |
|-----------------|------|-------------|
| `meta` | object | Nanosecond timestamp, source and schema version |
| `ports` | object | Per-DPDK-port standard counters and driver xstats, keyed by port ID |
| `entropy` | object | Shannon, min-entropy, mutual-information and flow metrics |
| `psd` | object | Rate power spectral density summary and bins |
| `observe` | object | Throughput, loss, CPU, memory and latency summary |
| `handshake` | object | Optional TCP handshake counters and rates |
| `log` | object | Current log text |

Schema version 2 replaces the ambiguous `tx_submit_abs_err_*`,
`tx_submit_early`, and `tx_submit_late` fields with deadline-overshoot and PMD
call-duration fields. It also replaces `sampler_dropped` with explicit current
sample and ring-overwrite counters.

Schema version 4 corrects min-entropy publication, changes `flow_distinct` to
a HyperLogLog estimate, and defines `mi_max_*` as bounds derived from observed
marginal entropy. Timing entropy now measures successful TX-burst intervals,
independent of which packets the sampler selects.

Schema version 5 adds `population_target` to every min-entropy diagnostic.

The CPU fields have distinct scopes. `process_cpu_cores` is aggregate process
CPU time divided by wall time. `enabled_lcore_utilization_ratio` divides that
value by `enabled_lcores`; it is not an individual PMD-worker utilization.
`cpu_busy_pct` remains only as a deprecated, capped compatibility field.
`target` is now the finite-window 95% lower threshold derived from the
population target and current sample count. Port targets combine the weighted
TCP, UDP, SCTP and no-port protocol populations instead of using one protocol's
range.

Schema version 6 adds the `informational` diagnostic state and corrects
configuration-derived targets for extension-protocol port ranges and distinct
VXLAN address/VNI populations.

Schema version 7 separates `psd.fundamental_hz` from
`psd.strongest_peak_hz`. The existing `psd.dominant_hz` remains a compatibility
alias for the strongest peak.

`entropy.min_diagnostics` contains one object per min-entropy dimension. Each
object publishes `measured`, `target`, `population_target`, `gap_bits`, `attainment`,
`dominance_ratio`, `max_probability`, `samples`, `distinct`, `max_count`,
`baseline_source`, and `state`. Consumers must treat `inactive` and
`insufficient-samples` and `informational` as non-comparable states rather than
numerical zeros. `informational` means the target was derived from observed
support because no configured population target exists.
See [Entropy Interpretation and Baselines](../concepts/entropy-interpretation.md)
for formulas and baseline semantics.

### `GET /metrics`
Prometheus / OpenMetrics text format.

**Response example:**
```
# HELP bless_tx_packets Total transmitted packets
# TYPE bless_tx_packets counter
bless_tx_packets{port="0"} 450000000
# HELP bless_entropy_shannon_h Shannon entropy of traffic distribution
# TYPE bless_entropy_shannon_h gauge
bless_entropy_shannon_h 12.87
...
```

Available metric prefixes: `bless_` (general stats), `bless_entropy_` (entropy metrics),
`bless_latency_` (latency).

### `GET /api/control`
Runtime control endpoint. Currently returns the current TSC cycle counter
(placeholder implementation).

**Response example:**
```
1719523456789
```

Note: The control plane is implemented on WebSocket (`cmd` protocol). This HTTP
endpoint is reserved and may be migrated in the future.

---

## WebSocket Control Protocol

Connect to `ws://<host>:8000/wsURL`. Clients send JSON-format commands; the server
pushes stats updates at 3 Hz broadcast.

Inbound commands must be one complete, final (`FIN=1`) text frame of at most
4096 bytes. The CivetWeb transport rejects larger declared payloads before
allocating their body; continuation, binary, empty, and embedded-NUL frames are
rejected. An oversized declared payload is closed at the transport boundary.
Validation and JSON errors that reach the application callback are returned
directly to the originating connection as an object such as:

```json
{"error":"fragmented WebSocket messages are not supported","code":400}
```

### Client -> Server

#### `{"cmd": "start"}`
Switches worker state to `STATE_RUNNING`, begins traffic generation.

Response: No direct response. The `state` field in the `stats.json` broadcast will
reflect the state change.

#### `{"cmd": "stop"}`
Switches worker state to `STATE_STOPPED`, stops traffic generation.

#### `{"cmd": "exit"}`
Sends exit signal. The server will clean up resources and terminate the process.

#### `{"cmd": "conf"}`
Queries the current config file path.

Response: No direct reply. `cfm->addr` is output in the log.

#### `{"cmd": "get"}`
Queries all current runtime parameters.

**Response example (via log/broadcast):**
```json
{
  "cmd": "config",
  "traffic_model": 0,
  "pps_rate": 1000000,
  "pps_burst": 64,
  "bps_rate": 1000000000,
  "bps_burst": 0,
  "batch": 64,
  "batch_delay_us": 0,
  "batch_jitter_us": 0,
  "num": -1,
  "sample_interval": 100,
  "entropy_target": 7.5,
  "entropy_dim": 8,
  "entropy_adapt_gain": 0.1,
  "hs_rate": 100,
  "hs_timeout_us": 1000000,
  "hs_mix_ratio": 0
}
```

#### `{"cmd": "set", "key": "<name>", "value": <number>}`
Requests an update to a single runtime parameter.

Runtime-mutable fields take effect through the field descriptor apply path.

Startup-only fields are rejected.

**Recognized parameters:**

| key | Type | Mutability | Description |
|-----|------|------------|-------------|
| `pps_rate` | uint32 | Runtime | Target transmit rate in packets per second |
| `pps_burst` | uint32 | Startup-only | Packet token-bucket capacity |
| `bps_rate` | uint32 | Startup-only | Target bit rate |
| `bps_burst` | uint32 | Startup-only | Bit-rate token-bucket capacity |
| `batch` | uint16 | Startup-only | TX burst size per round |
| `batch_delay_us` | uint64 | Startup-only | Inter-burst delay in microseconds |
| `batch_jitter_us` | uint64 | Startup-only | Inter-burst jitter in microseconds |
| `num` | int64 | Startup-only | Total packet count, with `-1` meaning unlimited |
| `traffic_model` | uint8 | Startup-only | Traffic model: 0 constant, 1 Poisson, 2 Pareto burst |
| `sample_interval` | uint32 | Runtime | Entropy sampling interval |
| `entropy_target` | double | Runtime | Adaptive entropy control target |
| `entropy_dim` | uint8 | Runtime | Adaptive entropy dimension, from 0 through 8 |
| `entropy_adapt_gain` | double | Runtime | Adaptive control gain |
| `hs_rate` | uint32 | Startup-only | Handshake-mode rate |
| `hs_timeout_us` | uint64 | Startup-only | Handshake timeout in microseconds |
| `hs_mix_ratio` | uint16 | Startup-only | Handshake traffic mix ratio |
| `seed` | uint64 | Next start | PRNG seed used when transmission next starts |

The descriptor table in `src/runtime_field.c` is the implementation source of
truth until this reference is generated from that table.

**Example:**
```json
{"cmd": "set", "key": "pps_rate", "value": 500000}
```

**Reply:** The `cmd` field in the command is replaced with a confirmation string,
e.g., `"set pps_rate=500000"`.

---

### Server -> Client (Broadcast)

The server broadcasts the full stats JSON to all connected WebSocket clients
every 333 ms.

When a WebSocket becomes ready, the server sends a welcome message:
```json
{"hello":"world"}
```

---

## Error Codes

| HTTP Status | Scenario |
|-------------|----------|
| 200 | Success |
| 404 | Requested resource not found (e.g., `/assets/chart.min.js` not generated) |
| 5xx | Internal server error |
