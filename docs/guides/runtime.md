# Runtime Control Parameters

Bless supports a bounded set of runtime parameter adjustments through
WebSocket.

The control panel is located at the bottom of the `/entropy` page in the
collapsed **Runtime Control** section.

## Mutability Boundary

The runtime-mutable fields are:

- `pps_rate`
- `sample_interval`
- `entropy_target`
- `entropy_dim`
- `entropy_adapt_gain`

`seed` is accepted but takes effect when transmission next starts.

The remaining recognized fields are startup-only and a WebSocket `set` request
for them is rejected.

Configure startup-only fields through YAML or CLI before launching Bless.

The descriptor table in `src/runtime_field.c` is the implementation source of
truth.

See the [API reference](../reference/api.md#websocket-control-protocol) for
the current mutability table.

## Usage

### Frontend Control Panel (recommended)

1. Open `http://<host>:8000/entropy`
2. Expand the **Runtime Control** section at the bottom of the page
3. Click **[Refresh All]** to fetch current runtime parameters
4. Enter new values in the input fields and click **Set** to apply

### Direct WebSocket Commands

```json
// Query current configuration
{"cmd": "get"}

// Set a parameter
{"cmd": "set", "key": "pps_rate", "value": 100000}

// Control state
{"cmd": "start"}    // Start packet transmission
{"cmd": "stop"}     // Stop packet transmission
{"cmd": "exit"}     // Exit the bless process
```

### Friendly Numeric Input

For parameters marked `suffix: true` (rate-type parameters), the following suffixes are supported:

| Input | Actual Value | Description |
|------|--------|------|
| `100000` | 100000 | Bare number |
| `100K` | 100000 | K = x1,000 |
| `1.5M` | 1500000 | M = x1,000,000 |
| `10G` | 10000000000 | G = x1,000,000,000 |
| `500k` | 500000 | Case-insensitive |

Non-rate parameters accept only bare numbers.

---

## Parameter Reference

### Pre-flight environment validation

`--preflight=warn|strict|off` controls read-only startup validation.  `warn` is
the default and never changes host settings.  `strict` rejects severe placement
problems such as cross-NUMA worker/NIC assignments or selecting both SMT
siblings as workers.  `--environment-dump=PATH` selects the machine-readable
JSON snapshot path (default `bless-environment.json`).  Missing sysfs data is
reported as unavailable rather than treated as a failure, which keeps container
and virtual-PMD runs usable.

The `observe.tx_submit_*` JSON fields and corresponding
`bless_tx_submit_*` Prometheus metrics measure burst submission against the
per-worker absolute TSC deadline produced by the fixed, jittered, Poisson, or
Pareto model. `tx_submit_overshoot_*` measures the delay between the deadline
and entry to `rte_eth_tx_burst()`. `tx_burst_duration_*` measures time spent in
that call. Percentiles use 1 us buckets through 255 us, 8 us buckets through
1023 us, and logarithmic buckets above that range. These metrics do not measure
NIC or wire departure time. Low-rate runtime-noise telemetry also samples
enabled-lcore frequency and cumulative process context switches outside the
packet hot path.

### 1. Rate Limiting

Token bucket rate limiter. All rate limiters execute at the end of the transmit queue and do not alter packet construction logic.

#### `pps_rate`

- **Type**: uint32
- **Unit**: pkt/s
- **Default**: 0 (unlimited)
- **Suffix support**: [OK] (K/M/G)
- **Function**: Maximum packets sent per second. 0 means unlimited.
- **Effect**: When the transmit rate exceeds the limit, excess packets are dropped.
- **Note**: Not equivalent to the physical transmit rate of the DPDK port. Rate limiting executes at the application layer.

#### `pps_burst`

- **Type**: uint32
- **Unit**: pkt
- **Default**: 0 (auto = `batch` x 4)
- **Suffix support**: [OK] (K/M/G)
- **Function**: Token bucket burst capacity (bucket depth). Allows temporarily exceeding `pps_rate` by this many packets.
- **Effect**: Larger values produce more "bursty" traffic. Recommended to match the `batch` value.

#### `bps_rate`

- **Type**: uint32
- **Unit**: bit/s
- **Default**: 0 (unlimited)
- **Suffix support**: [OK] (K/M/G)
- **Function**: Maximum bits sent per second. 0 means unlimited.
- **Effect**: `pps_rate` and `bps_rate` both apply; whichever limit is reached first triggers throttling. Recommend enabling only one (set the other to 0).

#### `bps_burst`

- **Type**: uint32
- **Unit**: bit
- **Default**: 0 (auto = 65536)
- **Suffix support**: [OK] (K/M/G)
- **Function**: Token bucket bit-level burst capacity.

---

### 2. Traffic

#### `batch`

- **Type**: uint16
- **Range**: 1 ~ 65535
- **Unit**: pkt
- **Default**: 0 (unset; `conf/config.yaml` uses 32, `--batch` without a value defaults to 256)
- **Function**: Number of packets sent per loop iteration (one `rte_eth_tx_burst` call).
- **Effect**: Larger values increase single DMA transfer efficiency but increase inter-batch latency. Typically set to 64 or 128.

#### `batch_delay_us`

- **Type**: uint64
- **Unit**: us (microseconds)
- **Default**: 0 (no delay)
- **Function**: Fixed delay between two batches of packets.
- **Effect**: Controls the overall transmit rate. Larger delay means lower PPS. Together with `batch`, determines average PPS:
  ```
  Average PPS ~ batch / (batch_delay_us x 1e-6)
  ```
  Example: `batch=64`, `delay=100000us` -> Average PPS ~ 640.
- **Interaction with `pps_rate`**: Both settings are maximum-rate constraints.
  Bless derives a per-batch interval from `pps_rate` and uses the larger
  (slower) of that interval and `batch_delay_us`. With `traffic_model=0`, a
  zero delay and no rate limit means best-effort maximum throughput. With
  `traffic_model=1` or `2`, a zero effective interval uses a 1000 us default
  distribution scale.

#### `batch_jitter_us`

- **Type**: uint64
- **Unit**: us (microseconds)
- **Default**: 0
- **Function**: Random jitter on top of `batch_delay_us` (uniform distribution [0, jitter]).
- **Effect**: Introduces inter-packet gap entropy. When `traffic_model=1` (Poisson model), this value is overridden by Poisson intervals.
- **Tuning**: Set to 10-50% of `batch_delay_us` for significant Delta TSC entropy.

#### `num`

- **Type**: int64
- **Range**: -1 (unlimited) or >= 0
- **Unit**: pkt
- **Default**: 0 (unset; `conf/config.yaml` uses 100; CLI `--num` without a value is 0)
- **Function**: Total number of packets to send. Automatically stops when this count is reached.
- **Effect**: `-1` = send indefinitely until a `stop` command is received or the process exits. `0` = zero-packet mode (no distribution needed).

#### `traffic_model`

- **Type**: uint8
- **Range**: 0 | 1 | 2
- **Unit**: none (enum)
- **Default**: 0
- **Function**: Inter-batch gap model selection.
  - `0` = **Fixed interval**: strictly scheduled by `batch_delay_us` + `batch_jitter_us`
  - `1` = **Poisson model**: inter-packet gaps follow an exponential distribution, simulating real network traffic
  - `2` = **Pareto ON-OFF model**: alternating heavy-tailed active and idle periods
- **Effect**: Poisson and Pareto modes increase timing variability through different traffic models.

#### `sample_interval`

- **Type**: uint32
- **Range**: 0 (disabled) or N >= 1
- **Unit**: pkt
- **Default**: 0 (sampler disabled; `conf/config.yaml` uses 10)
- **Function**: Step interval for entropy sampling. Samples one packet every N packets. 0 disables sampling entirely.
- **Effect**: Increasing the sampling interval reduces computational overhead but decreases statistical precision. 1 = per-packet sampling (most precise).
- **Note**: Sampling interval affects the count denominator for all entropy dimensions and mutual information.

---

### 3. Entropy

Adaptive rate limiting adjusts transmit rate from the gap between measured
entropy and the configured target.

#### `entropy_target`

- **Type**: double
- **Unit**: bit
- **Default**: 0.0 (adaptive disabled)
- **Range**: 0.0 ~ max_entropy (depends on configured IP/port ranges)
- **Function**: Target entropy value. When measured entropy is below this target, the transmit rate is automatically reduced to improve randomization quality.
- **Effect**: 
  - `0.0` = adaptive disabled, transmit at original rate
  - `> 0.0` = adaptive enabled. Uses the dimension specified by `entropy_dim` for feedback control

#### `entropy_dim`

- **Type**: uint8
- **Range**: 0 ~ 8
- **Unit**: none (enum)
- **Default**: 0
- **Function**: Entropy dimension used for adaptive feedback.
  - `0` = src_ip
  - `1` = dst_ip
  - `2` = src_port
  - `3` = dst_port
  - `4` = protocol
  - `5` = joint_5tuple
  - `6` = tcp_flags
  - `7` = pkt_size
  - `8` = delta_tsc

#### `entropy_adapt_gain`

- **Type**: double
- **Range**: 0.0 ~ 1000.0
- **Default**: 0.1
- **Unit**: none (proportional gain)
- **Function**: Gain coefficient for adaptive control. Higher gain means faster rate limiter response to entropy deviation.
- **Effect**:
  - 0.1 (default): slower response, prevents oscillation
  - 0.5: fast response, may produce overshoot
  - > 0.5: may cause violent transmit rate fluctuations

---

### 4. Handshake

Control parameters for TCP three-way handshake mode.

#### `hs_rate`

- **Type**: uint32
- **Unit**: conn/s
- **Default**: 0 (falls back to `batch` at worker init)
- **Suffix support**: [OK] (K/M/G)
- **Function**: TCP connections initiated per second (Connection Per Second).
- **Effect**: Controls the three-way handshake rate. Used to test gateway conntrack table handling capacity. Requires dual-instance operation (one side initiates, the other responds).

#### `hs_timeout_us`

- **Type**: uint64
- **Unit**: us (microseconds)
- **Default**: 0 (falls back to 10 s at worker init)
- **Function**: Timeout waiting for peer response. If no SYN-ACK or ACK is received within this time, the connection is considered timed out.
- **Effect**: Timed-out connections are counted in `timed_out`. Shorter timeouts result in faster conntrack table turnover.

#### `hs_mix_ratio`

- **Type**: uint16
- **Range**: 0 ~ 1000
- **Unit**: 0-1000 (corresponding to 0.0% ~ 100.0%)
- **Default**: 0
- **Function**: Ratio of stateful handshake traffic in each batch.
- **Effect**: 
  - `0` = almost all stateless traffic (the worker keeps one handshake slot)
  - `500` = 50% handshake + 50% regular traffic
  - `1000` = pure three-way handshake traffic
