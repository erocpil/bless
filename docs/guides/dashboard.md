# Entropy Dashboard: Data Reference

This document describes every data field displayed on the `/entropy` dashboard -- its meaning, calculation method, theoretical maximum, and diagnostic guidance.

This page covers **three entropy families** computed by Bless:
- **Shannon entropy** H(X) per dimension -- average unpredictability
- **Min-entropy** Hinf(X) -- worst-case (maximum-likelihood) unpredictability
- **Mutual Information** I(X;Y) -- statistical dependence between pairs of dimensions

Plus **flow-level entropy** (connection-table analysis) and the **MI correlation matrix** heatmap.

---

## Quick Reference: Entropy Dimensions

### Shannon Entropy (13 Dimensions)

| Dimension | What It Measures | Max (bits) |
|------|---------|----------|
| Protocol | Protocol distribution uniformity | log_2(#protocols) |
| Src IP | Source address space utilization | log_2(src_range) |
| Dst IP | Destination address space utilization | log_2(dst_range) |
| Src Port | Source port distribution | log_2(src_port_range) |
| Dst Port | Destination port distribution | log_2(dst_port_range) |
| Pkt Size | IMIX packet size diversity | log_2(#IMIX sizes) |
| TCP Flags | SYN/ACK/PSH/RST flag distribution | <= 8 |
| Delta TSC (Timing) | Inter-packet gap jitter | log_2(frame_count) |
| Joint 5-tuple | 5-tuple joint entropy | 32 (compressed key) |
| VXLAN Encap | VXLAN vs non-VXLAN ratio | 1 |
| Outer Src IP | Tunnel outer source IP distribution | log_2(outer_src_range) |
| Outer Dst IP | Tunnel outer destination IP distribution | log_2(outer_dst_range) |
| VNI | VXLAN Network Identifier distribution | log_2(#VNI) |

### Min-entropy (12 Dimensions)

Each dimension reports `Hinf = -log_2(p_max)` -- worst-case unpredictability, exposing the most frequently occurring value. Dimensions: protocol, src_ip, dst_ip, src_port, dst_port, pkt_size, tcp_flags, vxlan_encap, outer_src_ip, outer_dst_ip, vni, total_5tuple.

### Flow-Level Entropy (Handshake Mode)

| Dimension | Meaning |
|-----------|---------|
| Flow 5-tuple | Connection-level 5-tuple XOR distribution entropy |
| Flow Lifetime | Connection lifetime (TSC delta) distribution entropy |
| Flow Event | Event type entropy: CREATED / ESTABLISHED / TIMEOUT / RST |
| Distinct Flow Count | Unique flows / tracked packets / ratio |

> See below for detailed per-dimension calculation methods and diagnostic guidance.

---

## 1. Shannon Entropy (Average Unpredictability)

### 1.1 Formula

Computed by `shannon_from_sorted()` in `include/entropy.h`:

```
H(X) = - Sigma (n_i / N) * log_2(n_i / N)
```

Where:
- `N` = total samples in the current window (sorted array)
- `n_i` = run length of each distinct value in the sorted array

A single pass over the sorted array yields all run-lengths; no probability table is maintained. Runtime: O(N log N) due to `qsort`.

### 1.2 Composition Section (6 dimensions)

| Dim ID | Label | JSON key | Max (bits) | Description |
|--------|-------|----------|------------|-------------|
| `protocol` | Protocol | `entropy.protocol` | 8 | Distribution of IP protocol numbers (TCP=6, UDP=17, ICMP=1, SCTP=132, etc.) |
| `src_ip` | Src IP | `entropy.src_ip` | 32 | Distribution of source IPv4 addresses |
| `dst_ip` | Dst IP | `entropy.dst_ip` | 32 | Distribution of destination IPv4 addresses |
| `src_port` | Src Port | `entropy.src_port` | 16 | Distribution of source port numbers (TCP/UDP) |
| `dst_port` | Dst Port | `entropy.dst_port` | 16 | Distribution of destination port numbers |
| `joint_5tuple` | Joint 5-tuple | `entropy.joint_5tuple` | 64 | Shannon entropy of a compressed 5-tuple key: `(proto << 24) \| (src_ip>>24 & 0xff)<<16 \| (dst_ip>>24 & 0xff)<<8 \| (src_port & 0xff)` -- captures cross-dimension correlation within a single joint distribution. The difference `total_5tuple - joint_5tuple` measures how much redundancy exists between dimensions. |

#### 1.2.1 Diagnostic Guidance (Composition)

| Dim | Low H signal | Most Likely Cause | Check | Fix |
|-----|-------------|-------------------|-------|-----|
| `protocol` | H < 0.5 bits, only one proto dominates | Config specifies only one protocol, or weights are extremely skewed (e.g., tcp=999, udp=1) | `dist` weights in YAML or CLI `--tcp/--udp` args | Add or balance protocol weights |
| `src_ip` | H < 3 bits for a 1024-IP pool | IP pool too small, or interleave-depth too low causing short-term IP reuse | `src` range in config; `interleave-depth` value; `batch * workers > pool_size` | Increase IP range; raise interleave-depth to 70-100; reduce batch size |
| `dst_ip` | H < 2 bits for a 32-IP pool | Same causes as src_ip, but dst IPs are often fewer by design | `dst` range; verify all dst IPs appear in samples | Increase dst range or check if interleave cycles through all targets |
| `src_port` | H < 4 bits for default 100-port range | Port rotation is blocked by batch: all packets in a batch share the same port | `src` port range in config; `batch` size vs port count | Reduce batch or increase port range to ensure batch < port_count |
| `dst_port` | H < 2 bits for default 10-port range | Same as src_port; also possible that fixed-service ports (80, 443) are used exclusively | `dst` port range; protocol-specific port configs | Enlarge dst port range |
| `joint_5tuple` | significantly lower than sum of per-dim H values | Dimensions are correlated -- I(src_ip;dst_ip) or other MI pairs are high | MI matrix for saturated cells | Reduce per-dimension coupling (see MI §3.4) |

#### 1.2.2 Recommended Fill Targets (Shannon)

| Dim | Target utilization | Meaning |
|-----|-------------------|---------|
| `protocol` | > 70% of log_2(#configured_protos) | At least 3 protocols with balanced weights |
| `src_ip` | > 80% of log_2(src_range) | IP pool is well utilized |
| `dst_ip` | > 70% of log_2(dst_range) | Destination distribution is healthy |
| `src_port` | > 80% of log_2(src_port_range) | Ports are cycling properly |
| `dst_port` | > 70% of log_2(dst_port_range) | Target ports are distributed |
| `joint_5tuple` | > 60% of sum of above maxima | Overall 5-tuple coverage |

### 1.3 Behavioral Section (3 dimensions)

| Dim ID | Label | JSON key | Max (bits) | Description |
|--------|-------|----------|------------|-------------|
| `pkt_size` | Pkt Size | `entropy.pkt_size` | 16 | Variation in IP packet lengths (Total Length field) |
| `tcp_flags` | TCP Flags | `entropy.tcp_flags` | 8 | Diversity of TCP flag combinations (SYN, ACK, PSH, RST, FIN, etc.) |
| `delta_tsc` | Timing | `entropy.delta_tsc` | 64 | Inter-packet timing variation, measured in TSC ticks shifted right by 12 (approximately microsecond resolution) |

#### 1.3.1 Diagnostic Guidance (Behavioral)

**pkt_size**:
- **Low H signal**: H < 1 bit when IMIX is configured with 4+ sizes
- **Most likely cause**: Only one IMIX size is active, or IMIX is not configured (fixed packet size)
- **Check**: `ether.imix` in YAML config; verify constructor uses IMIX table
- **Fix**: Enable IMIX with at least 4 distinct size buckets; `bless.ether.mtu` should not be set to a single value that trims all packets to the same size

**tcp_flags**:
- **Low H signal**: H < 0.5 bits in TCP-mixed traffic
- **Most likely cause**: Pure UDP/ICMP traffic (no TCP flags sampled), or tx-only mode sends only PSH+ACK
- **Check**: Are TCP packets configured? Is `tcp_flags` distribution in `proto_tcp.c` uniform?
- **Fix**: Add TCP protocol weight; in handshake mode flags are naturally diverse (SYN, SYN-ACK, ACK, RST)

**delta_tsc**:
- **Low H signal**: H < 2 bits regardless of configured delay
- **Most likely cause**: Fixed inter-packet interval (`traffic_model=0`, `batch_jitter_us=0`)
- **Check**: `traffic_model` (0=fixed, 1=Poisson, 2=Pareto ON-OFF); `batch_jitter_us` value
- **Fix**: Set `traffic_model=1` (Poisson) for maximum timing entropy; or add `batch_jitter_us` for moderate jitter

### 1.4 Tunnel Section (4 dimensions)

| Dim ID | Label | JSON key | Max (bits) | Description |
|--------|-------|----------|------------|-------------|
| `vxlan_encap` | VXLAN Encap | `entropy.vxlan_encap` | 1 | Binary: whether packet carries VXLAN encapsulation (0/1). Entropy is 1.0 bit at 50/50 split, 0 at 0% or 100%. |
| `outer_src_ip` | Outer Src IP | `entropy.outer_src_ip` | 32 | Distribution of VXLAN outer-tunnel source IPv4 addresses (VTEP IPs) |
| `outer_dst_ip` | Outer Dst IP | `entropy.outer_dst_ip` | 32 | Distribution of VXLAN outer-tunnel destination IPv4 addresses |
| `vni` | VNI | `entropy.vni` | 24 | Distribution of VXLAN Network Identifiers |

#### 1.4.1 Diagnostic Guidance (Tunnel)

**vxlan_encap**:
- **Low H signal**: H < 0.1 bits
- **Most likely cause**: VXLAN is disabled (`vxlan.enable: false`), or ratio is 0 or 1000 (all or nothing)
- **Check**: `vxlan.enable` and `vxlan.ratio` in YAML config
- **Fix**: Set `vxlan.ratio` to 300-700 (30%-70%) for near-maximum binary entropy

**outer_src_ip**:
- **Low H signal**: H < 1 bit when multiple outer src IPs configured
- **Most likely cause**: Only one VTEP source address configured; or VXLAN ratio too low to accumulate enough samples
- **Check**: `vxlan.ether.type.ipv4.src` array; verify at least 2+ addresses
- **Fix**: Add multiple VTEP source addresses; increase `vxlan.ratio`

**outer_dst_ip**:
- **Low H signal**: H < 1 bit
- **Most likely cause**: All VXLAN packets targeting the same destination VTEP
- **Check**: `vxlan.ether.type.ipv4.dst` array size
- **Fix**: Add multiple destination VTEP addresses; verify outer dst IP rotation

**vni**:
- **Low H signal**: H < 1 bit
- **Most likely cause**: Only one VNI configured, or VNI is fixed regardless of outer IP pairing
- **Check**: VNI configuration in `vxlan` section
- **Fix**: Configure 4+ distinct VNIs; verify VNI rotates with outer IP combinations

### 1.5 Derived Composite

| Field | JSON key | Formula | Description |
|-------|----------|---------|-------------|
| `total_5tuple` | `entropy.total_5tuple` | `H(protocol) + H(src_ip) + H(dst_ip) + H(src_port) + H(dst_port) + H(pkt_size) + H(tcp_flags)` | **Sum** of per-dimension Shannon entropies. Assumes dimension independence (i.e., no mutual information). This is an *upper bound* on the true joint entropy. Used as a single-number quality indicator for 5-tuple randomness. |

**Note**: `total_5tuple` is *not* the joint entropy -- it is a sum. The true joint entropy of all 7 dimensions would require a 136-bit key (7x32 compressed), which is impractical to sort. The sum is a conservative upper bound: if any two dimensions are correlated, the actual joint entropy will be strictly less than the sum.

---

## 2. Min-Entropy (Worst-Case Unpredictability)

### 2.1 Formula

Also computed inside `shannon_from_sorted()` when the `min_h` output pointer is non-NULL:

```
Hinf(X) = -log_2(max p_i)
```

Where `p_i = n_i / N` is the proportion of the most frequent value. If fewer than 2 samples, Hinf = 0.0.

### 2.2 Relationship to Shannon Entropy

For any distribution:

```
Hinf(X) <= H(X) <= log_2(k)
```

Where `k` is the number of distinct values observed. Equality `H = Hinf` holds only for a **uniform distribution**. The gap `H - Hinf` measures distribution skew -- larger gaps mean a few values dominate while many others appear rarely.

**Example**: 1024 IP addresses, one IP seen in 50% of samples:
- H ~ -[0.5*log_2(0.5) + 0.5/1023*1023*log_2(0.5/1023)] ~ 0.5 + 0.5*log_2(1023) ~ 5.5 bits
- Hinf = -log_2(0.5) = 1.0 bit
- Gap = 4.5 bits -> highly skewed distribution

### 2.3 All 12 Min-Entropy Dimensions

| Dim ID | Label | JSON key | Frontend max (bits) | Derivation |
|--------|-------|----------|---------------------|------------|
| `min_protocol` | Protocol | `min_protocol` | 8 | log_2(256 possible protocols) |
| `min_src_ip` | Src IP | `min_src_ip` | 32 | log_2(2^3^2 IPv4 addresses) |
| `min_dst_ip` | Dst IP | `min_dst_ip` | 32 | log_2(2^3^2) |
| `min_src_port` | Src Port | `min_src_port` | 16 | log_2(2^1^6 port numbers) |
| `min_dst_port` | Dst Port | `min_dst_port` | 16 | log_2(2^1^6) |
| `min_pkt_size` | Pkt Size | `min_pkt_size` | 16 | log_2(65535 max packet length) |
| `min_tcp_flags` | TCP Flags | `min_tcp_flags` | 8 | log_2(256 flag combinations) |
| `min_delta_tsc` | Timing | `min_delta_tsc` | 64 | log_2(2^6^4 TSC delta) |
| `min_outer_src_ip` | Outer Src IP | `min_outer_src_ip` | 32 | log_2(2^3^2) |
| `min_outer_dst_ip` | Outer Dst IP | `min_outer_dst_ip` | 32 | log_2(2^3^2) |
| `min_vni` | VNI | `min_vni` | 24 | log_2(2^2^4 VNI space) |
| `min_total_5tuple` | Aggregate (7 dimensions) | `min_total_5tuple` | -- | **Sum**: Hinf(protocol) + Hinf(src_ip) + Hinf(dst_ip) + Hinf(src_port) + Hinf(dst_port) + Hinf(pkt_size) + Hinf(tcp_flags). Despite the legacy API field name, this is not joint 5-tuple entropy. |

**Note on total_5tuple exclusion**: delta_tsc, outer_src_ip, outer_dst_ip, and vni are *conditional* dimensions -- they depend on traffic mode (delta_tsc) or VXLAN ratio. They are excluded from the aggregate so the total can be compared across different configurations.

#### 2.3.1 Frontend Card Layout Asymmetry

The Min-Entropy and Shannon (Composition/Behavioral/Tunnel) sections cover nearly identical dimension sets, with three deliberate omissions:

| Missing From | Dimension | Reason |
|-------------|-----------|--------|
| Min-Entropy cards | `vxlan_encap` | Binary dimension (0/1). Min-entropy for a binary variable carries little actionable information: at 50/50 split Hinf = 1.0 (identical to Shannon), at 90/10 Hinf ~ 0.15 bits. The VXLAN encapsulation ratio is better read directly from the `vxlan.ratio` config parameter or the Shannon value. |
| Min-Entropy cards | `joint_5tuple` | Joint 5-tuple packs 4 fields into a 32-bit compressed key. The min-entropy of this packed key answers "what is the most frequent compressed key value?", which has no intuitive mapping back to individual IPs or ports. Per-dimension min-entropy is more actionable for diagnosis. |
| Shannon cards | `total_5tuple` | The Shannon sum `entropy.total_5tuple` exists in the JSON API (`/api/stats`) and Prometheus (`/metrics`), but no dedicated card is rendered for it on the frontend. The Min-Entropy version `min_total_5tuple` serves as the dashboard's single-number aggregate instead, because worst-case (min-entropy) is the more conservative and security-relevant metric. The Shannon total is available for automated monitoring via `/metrics`.

### 2.4 Diagnostic Guidance

The dashboard's configuration-aware target, gap, evidence and status model is
defined in [Entropy Interpretation and Baselines](../concepts/entropy-interpretation.md).
Static ranges below are fallback guidance only; they must not override an
inactive state or a target derived from the active workload configuration.

| Hinf range | Interpretation | Action |
|----------|---------------|--------|
| ~ 0 bits | Almost all values are identical | Check config range for that dimension; verify interleave is working |
| < log_2(range)/2 | Distribution is significantly skewed | Increase interleave depth; reduce batch size; verify all workers cycle through full pool |
| ~ log_2(range) | Near-uniform distribution | Optimal for that dimension |
| ~ theoretical max | Perfect uniformity | No action needed |

### 2.5 Example population targets

| Dimension | Target | Notes |
|-----------|--------|-------|
| `min_protocol` | >= 1.0 bits | Multi-protocol test |
| `min_src_ip` | >= 6.0 bits (64 IPs) | Primary entropy contributor |
| `min_dst_ip` | >= 3.0 bits (8 IPs) | Distribution across destinations |
| `min_src_port` | >= 5.0 bits (32 ports) | Ephemeral port diversity |
| `min_dst_port` | >= 3.0 bits (8 ports) | Service port diversity |
| `min_pkt_size` | >= 2.0 bits (4 sizes) | IMIX profile |
| `min_tcp_flags` | >= 1.5 bits | TCP mixed flow |
| `min_delta_tsc` | >= 3.0 bits | Poisson timing model |
| `min_total_5tuple` | Sum of active component targets | Marginal aggregate, not joint entropy |

---

## 3. Mutual Information Matrix

### 3.1 Formula

```
I(X;Y) = H(X) + H(Y) - H(X,Y)
```

Where `H(X,Y)` is the Shannon entropy of the joint distribution, computed by packing 64-bit keys: `(X << 32) | Y`.

### 3.2 12 MI Pairs (4 Tiers)

| # | Tier | ID | Pair | Joint Key Packing | Measures |
|---|------|----|------|-------------------|----------|
| 1 | 1 | `mi_sip_dip` | Src IP <-> Dst IP | `(src_ip << 32) \| dst_ip` | Source-destination affinity -- high MI means certain src IPs always talk to certain dst IPs (e.g., 1:1 mapping). Low = uniform mixing. |
| 2 | 1 | `mi_spt_dpt` | Src Port <-> Dst Port | `(src_port << 32) \| dst_port` | Port pair coupling -- high MI indicates asymmetric ports (e.g., ephemeral->well-known). Low = random port pairing. |
| 3 | 1 | `mi_proto_spt` | Protocol <-> Src Port | `(proto << 32) \| src_port` | Protocol-port binding -- high MI when TCP always uses certain src ports and UDP uses others. |
| 4 | 2 | `mi_size_dpt` | Pkt Size <-> Dst Port | `(pkt_size << 32) \| dst_port` | Service-specific payload sizes -- high MI when each dst port uses distinct packet sizes (e.g., DNS=small, FTP=large). |
| 5 | 2 | `mi_size_proto` | Pkt Size <-> Protocol | `(pkt_size << 32) \| proto` | Protocol-specific frame sizes (TCP vs UDP vs ICMP differ in typical size). |
| 6 | 2 | `mi_dtsc_proto` | Delta TSC <-> Protocol | `((Delta TSC>>12) << 32) \| proto` | Per-protocol timing patterns -- different protocols may have different inter-packet gaps. |
| 7 | 2 | `mi_dtsc_flow` | Delta TSC <-> Flow Key | `((Delta TSC>>12) << 32) \| flow_key` | **Temporal flow locality**. High = packets of the same flow cluster in time (interleave=off). Low = flows are interleaved (interleave=on). **Key metric for interleave tuning.** |
| 8 | 3 | `mi_tcpf_sz` | TCP Flags <-> Pkt Size | `(tcp_flags << 32) \| pkt_size` | Handshake vs data -- high MI when SYN packets are small and DATA packets are large (normal TCP behavior). |
| 9 | 3 | `mi_tcpf_spt` | TCP Flags <-> Src Port | `(tcp_flags << 32) \| src_port` | Per-flow flag usage -- different src ports using different TCP flag patterns. |
| 10 | 3 | `mi_tcpf_dpt` | TCP Flags <-> Dst Port | `(tcp_flags << 32) \| dst_port` | Service-specific flag patterns (e.g., port 80: all SYN; port 443: has FIN). |
| 11 | 4 | `mi_osip_odip` | Outer Src IP <-> Outer Dst IP | `(outer_src << 32) \| outer_dst` | VTEP pair affinity -- high MI when tunnel endpoints are fixed pairs. Low = VTEPs are randomly paired. |
| 12 | 4 | `mi_vni_osip` | VNI <-> Outer Src IP | `(vni << 32) \| outer_src` | VNI-to-VTEP mapping -- high MI when each VNI is tied to one tunnel source. Low = VNI is decoupled from tunnel endpoint. |

### 3.3 Marginal Source

For MI pairs 1-6 and 11-12, the marginal entropies H(X) and H(Y) come from the **global dimension arrays** (all samples in window).

For MI pairs 7-10, the marginals are **derived from the joint array** via `H_FIELD()` (extracting upper/lower 32 bits from the 64-bit joint keys and running Shannon entropy on the subset). This ensures marginals use exactly the same population as the joint, avoiding bias from the TCP-only or flow-only filtering.

### 3.4 Interpretation

The frontend coalesces WebSocket statistics to one browser animation frame and
updates only expanded sections. Collapsing the MI matrix, detail cards, entropy
groups, or Rate PSD therefore stops their DOM/Canvas rendering while collection
continues in Bless. Expanding a section immediately renders the latest cached
snapshot; no measurement data is lost.

Flow-Level entropy is handshake-only and is hidden in other injector modes.
Handshake entropy retains the latest window with at least two lifecycle
samples, while `flow_*_count` fields report evidence from the current window.

| MI range | Meaning | Diagnostic |
|----------|---------|------------|
| I ~ 0 | X and Y are independent | Randomization is working well for this pair |
| 0 < I < min(H(X),H(Y)) | Partial correlation | Some structure exists; acceptable unless target is maximum entropy |
| I ~ min(H(X),H(Y)) | X fully determines Y (or vice versa) | **Red flag** -- this pair has zero effective randomness on one side |

### 3.5 Mutual-information bounds

Each `mi_max_*` field reports the mathematical upper bound
`min(H(X), H(Y))` from the same observed population used by its MI estimate.
This keeps `MI <= mi_max` even when runtime mutation produces values outside
the configured pool. The card bar uses the smaller of that bound and its
display maximum.

### 3.6 MI Correlation Matrix (12x12 Heatmap)

The matrix displays all dimensions in a fixed order:

```
 0  Protocol     6  TCP Flags
 1  Src IP       7  Delta TSC (Timing)
 2  Dst IP       8  Outer Src IP
 3  Src Port     9  Outer Dst IP
 4  Dst Port    10  VNI
 5  Pkt Size    11  Flow Key
```

- **Diagonal cells**: Show the Shannon entropy H(X) for each dimension, colored in a blue gradient.
- **Off-diagonal cells with MI**: Show the computed I(X;Y) for the 12 defined pairs, colored in a green gradient.
- **Off-diagonal empty cells**: Marked as `--` (gray) -- no MI is computed for this pair.

**Color intensity**: `lightness = 10 + ratio * 38` where the MI ratio uses the upper bound `min(H(X), H(Y))`. Higher MI = more saturated green. Higher Shannon = more saturated blue.

---

## 4. Flow-Level Entropy

Relevant only in `handshake` mode (dual-instance TCP handshake test). Flow-level entropy measures **connection behavior** rather than packet-level randomness -- it evaluates what the conntrack table sees.

### 4.1 How Flow Tracking Works

Each worker core maintains 4096 HyperLogLog registers for estimating distinct sampled 5-tuples. Registers are merged across workers when statistics are published, so the estimate does not saturate at a small fixed hash-table capacity. A separate `flow_total_pkts` counter tracks total sampled packets.
Because HyperLogLog is an estimator, its raw result can be slightly larger than
the number of sampled packets. Bless bounds the published estimate by
`flow_total`, preserving `flow_distinct <= flow_total` and `flow_ratio <= 1`.

On stats generation, registers and counters are read atomically and merged. The distinct-flow estimate is cumulative for the life of the process.

### 4.2 Flow 5-Tuple Entropy

| Field | JSON key | Max (bits) | Description |
|-------|----------|------------|-------------|
| Flow 5-tuple entropy | `flow_entropy_5tuple` | 64 | Shannon entropy of XOR-compressed flow keys `proto ^ src_ip ^ dst_ip ^ src_port ^ dst_port` from the connection table |

- **Meaning**: Measures the diversity of distinct connections seen by the conntrack table. Unlike packet-level `joint_5tuple` (which samples every Nth packet), this is connection-level: each entry represents one unique 5-tuple in the flow table.
- **Calculation**: XOR-compressed keys are sorted and scanned for run-lengths, identical to other Shannon entropy dimensions.
- **Typical values**:
  - `handshake` mode with 64+ CPS, 100+ concurrent connections: 8-16 bits
  - Single-flow test: < 1 bit
- **Diagnosis**: If flow_entropy_5tuple is significantly lower than packet-level joint_5tuple, the connection table is seeing less diversity than the injection engine is generating -- possible causes are conntrack hash collisions or session reuse.
- **Config association**: `hs_rate` (CPS), `hs_timeout_us` (connection lifetime), number of distinct 5-tuple pairs in `src/dst` IP/port ranges.

### 4.3 Flow Lifetime Entropy

| Field | JSON key | Max (bits) | Description |
|-------|----------|------------|-------------|
| Flow lifetime entropy | `flow_entropy_lifetime` | 64 | Shannon entropy of connection lifetime durations (TSC delta between CREATED and CLOSED) |

- **Meaning**: Variation in how long connections live. Real traffic has mixed-duration connections; uniform lifetimes are detectable and unrealistic.
- **Calculation**: For each connection, track TSC at CREATED event, compute `delta = (close_tsc - open_tsc) >> 12` at TIMEOUT/RST, then sort and compute Shannon entropy.
- **Typical values**:
  - All connections same timeout duration: H < 1 bit
  - Mixed timeout + early RST: 2-6 bits
- **Diagnosis**: Low lifetime entropy means all connections follow the same lifecycle (all timed out at `hs_timeout_us`, no early RSTs). This does not stress the conntrack GC path which must handle mixed lifetimes.
- **Config association**: `hs_timeout_us` (max age), handshake partner behavior (whether it sends RST or lets connections time out).
- **Fix**: Run with a handshake partner that randomly closes connections early (simulating real client behavior), rather than letting all connections time out.

### 4.4 Flow Event Entropy

| Field | JSON key | Max (bits) | Description |
|-------|----------|------------|-------------|
| Flow event entropy | `flow_entropy_event` | 2 (4 event types) | Shannon entropy of event type distribution: CREATED / ESTABLISHED / TIMEOUT / RST |

- **Meaning**: Are all event types happening at roughly the same rate, or is one event type dominating?
- **Typical values**:
  - Normal three-way handshake with timeout termination: ESTABLISHED and TIMEOUT dominate -> H ~ 1.0 bits
  - With early RSTs from partner: all 4 event types present -> H ~ 1.5-2.0 bits
  - Only CREATED events (no completion): H ~ 0 bits
- **Diagnosis**: Low event entropy indicates an incomplete handshake cycle. If only CREATED events appear, the handshake partner is not responding. If TIMEOUT dominates (H < 0.5), all connections age out, indicating either partner is not processing SYNs or hs_timeout_us is too short.
- **Fix**: Verify handshake partner is running and reachable; adjust `hs_timeout_us` to match network RTT; enable `hs_mix_ratio` to add non-handshake traffic variety.

### 4.5 Flow Details

| Field | JSON key | Description |
|-------|----------|-------------|
| Distinct Flows | `flow_distinct` | HyperLogLog estimate of unique sampled 5-tuple flows |
| Total Packets | `flow_total` | Total packets sampled for flow tracking |
| Flow Ratio | `flow_ratio` | `flow_distinct / flow_total` -- ratio of unique flows to total packets. |

- **Distinct Flows**: Approximate count of unique sampled flow keys. HyperLogLog avoids the former 2048-entry saturation limit while using fixed memory.
- **Flow Ratio**: A high ratio (> 0.5) means most packets belong to new flows -- high flow churn, stressing conntrack allocation and GC. A low ratio (< 0.1) means the same flows are reused repeatedly -- conntrack cache is warm, throughput will be higher but the test is not stressing the table.
- **Diagnosis**: 
  - Ratio > 0.8: Almost every packet is a new flow. Conntrack is under maximum insert pressure. This is the conntrack worst case.
  - Ratio < 0.2: Flow reuse dominates. Good for measuring cache-hot throughput, not for table pressure.
  - Ratio consistently 1.0: Most sampled packets carry a previously unseen flow key; allow for normal estimator error on small populations.
- **Config association**: `hs_rate` (connections per second), `hs_timeout_us` (how long connections stay in table), IP/port range sizes, `interleave-depth` (whether flows are temporally localized).

---

## 5. Calculation Architecture

### 5.1 Sampling

- Each worker core samples packets at a configurable rate (`sample_interval`, default every 10th packet).
- Min-entropy diagnostics retain the configured population target but compare
  each window against a 95% finite-sample threshold. This avoids treating
  unavoidable collisions in a sparse sample as generator bias.
- The dashboard reports the samples used in the current calculation and unread
  samples overwritten since the previous calculation. An overwrite means the
  newest ring window was retained; it is not a packet drop.
- Sampled packet fields are written to a **lock-free ring buffer** per worker (`ENTROPY_RING_SIZE=4096`)
- Write side: worker core does a single atomic store
- Read side: main lcore polls all worker buffers

### 5.2 Computation

Computation runs exclusively on the **main lcore** (not on worker cores):

1. **Collect**: Read all worker ring buffers into per-dimension arrays
2. **Sort**: Call `qsort()` with dimension-specific comparator (`cmp_u32` or `cmp_u64`) -- ~24 sorts per stats window
3. **Scan**: Single pass through each sorted array to compute run-lengths and accumulate Shannon + min-entropy contributions
4. **Joint**: For MI, pack 64-bit joint keys and repeat the sort+scan
5. **Smooth** (optional): Apply EMA smoothing to 12 diagonal entropy dimensions before broadcast

Total overhead: ~10-30ms per 1-10s stats window (negligible at 10M+ PPS).

### 5.3 Broadcast

The control plane (civetweb thread) broadcasts stats via WebSocket at 3 Hz. The entropy/MI computation runs on a *longer* interval (every 1-10s, default 1s). Between computations, the last computed values are re-broadcast.

---

## 6. Config-Derived Theoretical Max Values

When available from the YAML configuration, the dashboard derives per-dimension theoretical maxima:

| Field | JSON key | Derivation |
|-------|----------|------------|
| Max Src IP | `max_src_ip` | `log_2(config.src_ip_range)` or 32.0 |
| Max Dst IP | `max_dst_ip` | `log_2(config.dst_ip_range)` or 32.0 |
| Max Src Port | `max_src_port` | `log_2(config.src_port_range)` or 16.0 |
| Max Dst Port | `max_dst_port` | `log_2(config.dst_port_range)` or 16.0 |
| Max Outer Src IP | `max_outer_src_ip` | `log_2(VXLAN outer_src count)` or 0.0 |
| Max Outer Dst IP | `max_outer_dst_ip` | `log_2(VXLAN outer_dst count)` or 0.0 |
| Max VNI | `max_vni` | `log_2(VNI config count)` or 0.0 |

These values feed into the bar fill percentage on each Shannon entropy card: `fill% = (value / max) x 100`.

The frontend also has hard-coded absolute maxima (32 bits for IP, 16 for port, 8 for protocol, etc.) which are used when config-derived max is unavailable or larger than the absolute max.

---

## 7. JSON API

### 7.1 `/api/stats` Response Fields

**Shannon entropy** (inside `entropy` object):
```
entropy.protocol, entropy.src_ip, entropy.dst_ip, entropy.src_port, entropy.dst_port,
entropy.pkt_size, entropy.vxlan_encap, entropy.outer_src_ip, entropy.outer_dst_ip,
entropy.outer_src_port, entropy.vni, entropy.tcp_flags, entropy.delta_tsc,
entropy.total_5tuple, entropy.joint_5tuple
```

**Config-derived maxima**:
```
max_src_ip, max_dst_ip, max_src_port, max_dst_port, max_outer_src_ip,
max_outer_dst_ip, max_vni
```

**Min-entropy** (top-level):
```
min_protocol, min_src_ip, min_dst_ip, min_src_port, min_dst_port, min_pkt_size,
min_tcp_flags, min_delta_tsc, min_outer_src_ip, min_outer_dst_ip, min_vni,
min_total_5tuple
```

**Mutual information** (top-level):
```
mi_sip_dip, mi_spt_dpt, mi_proto_spt, mi_size_dpt, mi_size_proto,
mi_dtsc_proto, mi_dtsc_flow, mi_tcpf_sz, mi_tcpf_spt, mi_tcpf_dpt,
mi_osip_odip, mi_vni_osip
```

**MI theoretical maxima**:
```
mi_max_sip_dip, mi_max_spt_dpt, mi_max_proto_spt, mi_max_size_dpt,
mi_max_size_proto, mi_max_dtsc_proto, mi_max_dtsc_flow, mi_max_tcpf_sz,
mi_max_tcpf_spt, mi_max_tcpf_dpt, mi_max_osip_odip, mi_max_vni_osip
```

**Flow-level**:
```
flow_distinct, flow_total, flow_ratio, flow_entropy_5tuple,
flow_entropy_lifetime, flow_entropy_event, flow_count, sampler_samples,
sampler_overwritten, sampler_overwritten_window
```

**Pacing timing** (inside `observe`):
```
tx_submit_target_us, tx_submit_overshoot_p50_us,
tx_submit_overshoot_p99_us, tx_submit_overshoot_max_us,
tx_burst_duration_p50_us, tx_burst_duration_p99_us,
tx_burst_duration_max_us, tx_submit_samples
```

### 7.2 Prometheus `/metrics` Labels

All entropy values are exported as Prometheus-style text at `/metrics`:

- `bless_entropy{type="protocol|src_ip|dst_ip|src_port|dst_port|pkt_size|vxlan_encap|outer_src_ip|outer_dst_ip|tcp_flags|delta_tsc|total_5tuple|joint_5tuple|vni"}`
- `bless_entropy_max{type="src_ip|dst_ip|src_port|dst_port|outer_src_ip|outer_dst_ip|vni"}`
- `bless_entropy_min{type="protocol|src_ip|dst_ip|src_port|dst_port|pkt_size|tcp_flags|delta_tsc|outer_src_ip|outer_dst_ip|vni|total_5tuple"}`
- `bless_mutual_info{pair="sip_dip|spt_dpt|proto_spt|size_dpt|size_proto|dtsc_proto|dtsc_flow|tcpf_sz|tcpf_spt|tcpf_dpt|osip_odip|vni_osip"}`
- `bless_mutual_info_max{pair="sip_dip|spt_dpt|proto_spt|size_dpt|size_proto|dtsc_proto|dtsc_flow|tcpf_sz|tcpf_spt|tcpf_dpt|osip_odip|vni_osip"}`
- `bless_flow{type="distinct|total|ratio"}`
- `bless_flow_entropy{type="5tuple|lifetime|event"}`

---

## 8. Interpreting the Dashboard

### 8.1 Quick Diagnostic Flow

```
1. Check min_total_5tuple against its published window target
   |
2. Scan individual min-entropy cards for ~0 values
   -> dimension has zero variation -- check config
   |
3. Scan MI matrix for saturated cells
   -> two dimensions are strongly correlated
   |
4. Check mi_dtsc_flow specifically
   -> high = flows cluster in time (interleave too low)
   -> low = flows properly interleaved
   |
5. Verify Shannon values against config-derived maxima
   -> gap > 50% means pool is underutilized
```

### 8.2 Common Low-Entropy Causes

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| min_src_ip or min_dst_ip ~ 0 | All packets use same IP | Check `src-range` / `dst-range` config; increase pool size |
| min_protocol ~ 0 | Single protocol (all TCP) | Add `--udp=N --icmp=N` weights |
| min_pkt_size ~ 0 | Fixed packet size | Configure IMIX profile with multiple sizes |
| min_delta_tsc ~ 0 | Fixed inter-packet interval | Enable Poisson timing model: `traffic_model=1` |
| VXLAN dimension ~ 0 | VXLAN ratio too low or all same VNI | Increase `vxlan.ratio`; add multiple outer IPs/VNIs |
| mi_dtsc_flow > 2.0 bits | Flows clustered in time | Increase `interleave-depth` |
