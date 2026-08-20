# Bless Configuration Guide

Bless supports two configuration methods: **YAML files** (recommended) and **CLI parameters** (overrides / temporary adjustments).

---

## 1. YAML Configuration (Local Persistence)

All functional parameters are set via the `injector:` section. Below is a complete example with explanations:

```yaml
injector:
  # === DPDK Port & Queue ===
  p: 0x1              # Port bitmap (hexadecimal)
  q: 1                # Queues per port
  P:                  # Promiscuous mode (empty = off)
  T: 1                # Statistics interval (seconds, 0 = off)

  # === Traffic Control ===
  auto-start: true    # Auto-start transmission after launch
  mode: tx-only       # tx-only | rx-only | fwd | handshake
  num: -1             # Total packets per core (-1 = unlimited)
  batch: 64           # Burst size per round
  batch-delay-us: 100000  # Delay between rounds (microseconds)
  batch-jitter-us: 50     # Delay jitter (+/- microseconds, 0 = no jitter)
  traffic-model: 1    # Traffic model: 0 = uniform spacing, 1 = Poisson (exponential distribution)

  # === Rate Limiting ===
  pps-rate: 10000     # Max packet rate (0 = off)
  pps-burst: 512      # PPS burst limit (0 = auto = batch*4)
  bps-rate: 12500000  # Max byte rate, e.g., 100 Mbps (0 = off)
  bps-burst: 2097152  # BPS burst limit (0 = auto = 65536)

  # === TCP Handshake Mode ===
  hs-rate: 64         # SYNs per round (0 = auto = batch)
  hs-timeout-us: 10000000  # Connection idle timeout (microseconds, default 10s)
  hs-mix-ratio: 600   # Handshake permillage: 1000 = pure handshake, 0 = pure stateless

  # === Runtime Entropy Sampling ===
  sample-interval: 1  # Sample 5-tuple every N packets (0 = off, 1 = every packet)
  entropy-target: 12  # Adaptive rate-limiting target entropy (bits, 0 = off)
  entropy-dim: 2      # Target dimension: 0 = src_ip, 1 = dst_ip, 2 = src_port...
  entropy-adapt-gain: 0.1  # Proportional gain

  # === Protocol Ratios ===
  arp: 1
  icmp: 2
  tcp: 8
  udp: 9
```

> **Note**: The protocol weights in the `injector` section are equivalent to CLI arguments `--arp=1 --icmp=2 --tcp=8 --udp=9`. New extended protocol weights (e.g., SCTP) can be set via `--weight sctp=30` CLI or the `bless.ether.dist` YAML setting (see below).

### Weights and Distribution (`bless.ether.dist`)

Use `bless.ether.dist` to declare all protocol weights (both built-in and extended):

```yaml
bless:
  ether:
    # dist supports both Mapping and Sequence formats:
    dist:
      tcp: 50
      udp: 30
      sctp: 20
    # or: dist: [ tcp: 50, udp: 30, sctp: 20 ]
```

Weight values determine the traffic ratio of each protocol, allocated via the Largest Remainder algorithm.

### CLI Weight Overrides

```bash
# General syntax (recommended, supports all registered protocols including extensions):
bless conf/config.yaml -- --weight tcp=50 --weight sctp=30 --weight gre=20

# Legacy syntax (backward compatible, built-in types only):
bless conf/config.yaml -- --tcp=50 --sctp=30
```

### Protocol Packet Parameters (`bless:` Section)

Packet content (MAC / IP / port / VXLAN / anomaly injection) is configured in the `bless:` section:

```yaml
bless:
  hw-offload: [ ipv4, udp, tcp ]
  ether:
    dst: 02:00:00:00:00:01
    src:                    # empty = use port MAC
    copy-payload: true
    type:
      ipv4:
        src: 10.0.0.1+1024     # range syntax: base+count
        dst: [ 192.168.1.1 ]   # array syntax
    tcp:
      src: 10000+100
      dst: 80
      payload: "tcp payload text"
```

IP/port supports two syntaxes:
- **Range**: `172.16.1.1+10` or `10000+100`
- **Array**: `[ 10.0.0.1, 20.0.0.2 ]`

### Launch

```bash
# Use default config file conf/config.yaml
./build/release-static/bin/bless

# Specify a config file
./build/release-static/bin/bless conf/config-test.yaml

# YAML + CLI overrides (see below)
./build/release-static/bin/bless conf/config.yaml -- --traffic-model=1
```

---

## 2. CLI Overrides (Temporary Adjustments)

Parameters after the `--` separator take priority over the same-name parameters in YAML. Follows DPDK convention:
before `--` is the program name + config path, after `--` are application parameter overrides.

```bash
# Basic usage (recommended -- with -- separator)
bless conf/config-test.yaml -- --traffic-model=1

# Convenience form (compatible, without --)
bless conf/config-test.yaml --traffic-model=1

# Example 1: Poisson traffic model + high entropy target
bless conf/config-test.yaml -- \
  --traffic-model=1 \
  --entropy-target=14 \
  --entropy-dim=0 \
  --entropy-adapt-gain=5

# Example 2: UDP flow + PPS rate limiting
bless conf/config.yaml -- \
  --mode=tx-only \
  --udp=10 \
  --pps-rate=50000 \
  --pps-burst=1024

# Example 3: Handshake stress test + mix ratio
bless conf/config-hs-left.yaml -- \
  --hs-rate=200 \
  --hs-timeout-us=5000000 \
  --hs-mix-ratio=800
```

### Complete CLI Parameter Table

| Parameter | Type | Default | Description |
|-----------|------|--------|-------------|
| `-p` | hex mask | -- | Port bitmap |
| `-P` | flag | off | Promiscuous mode |
| `-q` | int | 1 | Queues per port |
| `-T` | sec | 0 | Statistics interval in seconds. `0` generates stats every 100 ms control loop (most frequent); `conf/config.yaml` uses 1 |
| `--auto-start` | bool | true | Auto-start transmission after launch |
| `--mode` | string | tx-only | Mode: tx-only / rx-only / fwd / handshake |
| `--num` | int | 0 | Total packets (`-1` = unlimited; `conf/config.yaml` uses 100) |
| `--batch` | int | 256 | Burst size per round |
| `--batch-delay-us` | int | 0 | Inter-round delay (microseconds) |
| `--batch-jitter-us` | int | 0 | Delay jitter (+/- microseconds) |
| `--traffic-model` | int | 0 | 0 = uniform, 1 = Poisson |
| `--sample-interval` | int | 10 | Sampling interval (0 = off) |
| `--arp` | int | -- | ARP weight |
| `--icmp` | int | -- | ICMP weight |
| `--tcp` | int | -- | TCP weight |
| `--udp` | int | -- | UDP weight |
| `--pps-rate` | int | 0 | PPS cap (0 = off) |
| `--pps-burst` | int | 0 | PPS burst cap |
| `--bps-rate` | int | 0 | BPS cap (0 = off) |
| `--bps-burst` | int | 0 | BPS burst cap |
| `--hs-rate` | int | 0 | SYNs per round (0 = auto) |
| `--hs-timeout-us` | int | 0 (falls back to 10 s) | Connection timeout (microseconds) |
| `--hs-mix-ratio` | int | 0 | Handshake permillage (0-1000) |
| `--entropy-target` | float | 0 | Adaptive rate-limit target entropy (bits, 0 = off) |
| `--entropy-dim` | int | 0 | Target dimension index |
| `--entropy-adapt-gain` | float | 0.1 | Proportional gain |

---

## 3. Preset Configuration Scenarios

| Config File | Scenario | Protocols | Special Parameters |
|-------------|----------|-----------|---------------------|
| `config-test.yaml` | Local stress test (lo interface) | TCP+UDP | sample-interval=10 |
| `config.yaml` | Physical NIC stress test | UDP | Includes VXLAN |
| `config-entropy-demo.yaml` | Entropy analysis demo | ARP+ICMP+TCP+UDP | IMIX, large IP range |
| `config-rate-pps.yaml` | PPS rate limiting | ARP+ICMP+TCP+UDP | pps-rate=10000 |
| `config-rate-bps.yaml` | BPS rate limiting | ARP+ICMP+TCP+UDP | bps-rate=12500000 |
| `config-rate-cps.yaml` | CPS handshake | TCP handshake | hs-rate=1000, pps-rate=15000 |
| `config-hs-left.yaml` | Handshake (left side) | TCP handshake | hs-rate=64 |
| `config-hs-right.yaml` | Handshake (right side) | TCP handshake | hs-rate=64 |
| `config-hs-mix-left.yaml` | Mixed handshake (left) | TCP handshake+stateless | hs-mix-ratio=600 |
| `config-hs-mix-right.yaml` | Mixed handshake (right) | TCP handshake+stateless | hs-mix-ratio=600 |
| `config-tcp-flags.yaml` | TCP flag diversity | TCP | Custom payload |

The pure-handshake pair is documented as a complete local veth experiment in
[Dual-Endpoint Handshake and Observe Experiment](handshake-experiment.md).

---

## 4. Deterministic Replay

`--seed=0xDEADBEEF` fixes the xorshift64* PRNG seed. The same seed produces identical
traffic patterns. All entropy, MI, and flow-level metrics are bit-reproducible.

```bash
bless config.yaml -- --seed=42 --num=1000000
# Run twice -> identical bless_entropy, MI matrix, pcap output
```

The seed is printed at startup (`PRNG seed: 42 (deterministic)`).

### Deterministic Scope

Only the PRNG-driven fields are deterministic.  The following are **not**
reproducible across runs:

- **TSC-derived fields**: `delta_tsc`, `min_delta_tsc`, and any MI pair
  involving `dtsc` (`mi_dtsc_proto`, `mi_dtsc_flow`) are hardware-timed and
  vary across runs.  The CI deterministic check (`tools/ci_det_check.py`)
  strips these fields before comparison.
- **RDTSC-based timestamps**: Latency histogram percentiles depend on CPU
  frequency and are not seed-determined.
- **Wall-clock fields**: `build_time`, `tsc_cycles`, and any `rte_rdtsc()`-derived
  values.

**Build reproducibility**: `SOURCE_DATE_EPOCH` is set during CI builds to
produce identical `BUILD_TIME` and `BUILD_HOST` fields across builds of the
same commit.

### CI Deterministic Check

`tools/ci_det_check.py` runs two BLESS instances with the same seed, compares
JSON output, and strips known non-deterministic fields:

- TSC-derived: `delta_tsc`, `min_delta_tsc`, `mi_dtsc_proto`, `mi_dtsc_flow`
- Observed MI bounds: `mi_*_max` fields
- Timestamps: `tsc_cycles`, `build_time`

### pcap Export

Bless uses DPDK's built-in `net_pcap` PMD directly, with no additional code:

```bash
bless --vdev net_pcap0,tx_pcap=/tmp/out.pcap config.yaml -- --mode tx-only
# tcpdump -r /tmp/out.pcap
```

---

## 5. DPDK EAL Configuration (`dpdk:` Section)

The `dpdk:` YAML section maps directly to DPDK EAL command-line arguments.
Each key-value pair is converted to the corresponding `--key=value` form
(single-character keys use `-k v` syntax; sequence-valued keys produce
comma-joined `--key=v1,v2`).

```yaml
dpdk:
  main-lcore: 16          # --main-lcore=N   (primary lcore for control thread)
  l: 0-16                 # -l CORELIST      (DPDK 20.11+ lcore list)
  n: 4                    # -n N             (memory channels)
  lcores: null            # --lcores         (per-lcore role map, rarely used)
  socket-mem: null        # --socket-mem     (per-socket memory, e.g. "1024,0")
  allow:                  # --allow (-a)     (PCI whitelist)
    - 0000:00:05.0
    - 00:06.0
  block: null             # --block (-b)     (PCI blacklist)
  vdev: null              # --vdev           (virtual device, e.g. net_pcap0)
  proc-type: primary      # --proc-type      (primary|secondary|auto)
  log-level: info         # --log-level      (DPDK log level)
  trace: regex-match      # --trace          (trace point regex)
  trace-dir: /tmp         # --trace-dir      (trace output directory)
  trace-bufsz: 100M       # --trace-bufsz    (trace buffer size)
  trace-mode: discard     # --trace-mode     (overwrite|discard)
  huge-dir: /dev/hugepages  # --huge-dir     (hugetlbfs mount point)
  file-prefix: null       # --file-prefix    (multi-process namespace)
```

### EAL keys

| Key | EAL Arg | Type | Notes |
|-----|---------|------|-------|
| `main-lcore` | `--main-lcore` | int | Lcore for control-plane thread (stats, WebSocket). Must not overlap worker lcores. |
| `l` | `-l` | string | Core list syntax: `0-16`, `0,2,4,6`. DPDK 20.11+. Legacy `-c COREMASK` also accepted. |
| `n` | `-n` | int | Number of memory channels (typically 4). |
| `allow` | `-a` / `--allow` | sequence | PCI BDF whitelist. Order here may differ from `RTE_ETH_FOREACH_DEV()` enumeration. Use `null` to allow all. |
| `vdev` | `--vdev` | string | Virtual PMD. e.g. `net_pcap0,tx_pcap=tx.pcap` for pcap export. |
| `proc-type` | `--proc-type` | string | `primary` (default) or `secondary` for multi-process. |
| `log-level` | `--log-level` | string | DPDK log level per component: `info`, `pmd.net.af_xdp:debug`, etc. |
| `file-prefix` | `--file-prefix` | string | Namespace prefix for shared memory objects (multi-process). |

The `allow` list order is not guaranteed to match DPDK's
`RTE_ETH_FOREACH_DEV()` enumeration order. Verify port numbering with the
startup log (`Port X: ...`).

### PCI Address Discovery

```bash
# List all NIC PCI addresses
ls -l /sys/class/net/*/device

# Filter to DPDK-compatible devices
dpdk-devbind.py --status
```

Parameters set here are passed directly to `rte_eal_init()` before any
application logic runs.  The `injector:` and `bless:` sections are processed
after EAL initialisation.

---

## 6. Parameter Priority

```
YAML dpdk:      -> DPDK EAL parameters (see section 5, passed to rte_eal_init)
YAML injector:  -> Injection parameters (converted to CLI-level via internal --)
CLI overrides   -> Parameters after `--`, override YAML injector values of same name (highest priority)
YAML bless:     -> Packet recipe parameters (MACs / IPs / ports / VXLAN / anomalies, YAML only)
```

CLI first processes the key-value pairs from YAML injector, then appends overrides from the command line. For the same parameter name, the later value wins.
