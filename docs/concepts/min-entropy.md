# Min-Entropy: Meaning and Tuning Guide

## What Is Min-Entropy

Min-entropy Hinf is defined as:

```
Hinf(X) = -log_2(max p_i)
```

That is, the negative logarithm of the probability of the most frequent observed value. It measures **"worst-case unpredictability"** -- if an attacker can guess only one value, what is their probability of being correct.

Despite its name, this is not the minimum of multiple entropy measurements
over an observation window. Configuration-aware targets, gap bits and evidence
states are specified in [Entropy Interpretation and Baselines](entropy-interpretation.md).

Unlike Shannon entropy H(X) = -Sigma p_i*log_2(p_i), which is "average unpredictability," min-entropy is "worst-case." For gateway stress testing, min-entropy is stricter than Shannon entropy -- **an attacker always targets your weakest link**.

### Value Range

| Hinf | Meaning |
|----|------|
| ~ 0 bits | Almost entirely the same value; an attacker guesses correctly with eyes closed |
| ~ log_2(N) bits | N values uniformly distributed; information-theoretically maximal randomness achieved |
| Intermediate values | Distribution is skewed; the more skewed, the lower Hinf |

For example, if src_ip has 1024 candidate values and is perfectly uniform, Hinf = 10.0 bits; if one IP accounts for 50% of traffic, Hinf ~ 1.0 bit (since -log_2(0.5) = 1).

---

## Per-Dimension Guide

### 1. min_protocol -- Protocol Distribution

- **Meaning**: Worst-case diversity of packet IP protocol numbers (TCP=6, UDP=17, ICMP=1)
- **Theoretical maximum**: Based on the number of protocol types used in the config; ceiling 8 bits (max 256 protocol types)
- **Typical values**:
  - Single-protocol traffic (only UDP) -> Hinf ~ 0
  - Three protocols uniform -> Hinf ~ 1.58
- **Tuning**: If Hinf < 0.5, increase the proportion of low-frequency protocols

### 2. min_src_ip / min_dst_ip -- Source/Destination IP Diversity

- **Meaning**: Proportion of the most frequent address in the source/destination IP distribution
- **Theoretical maximum**: log_2(IP pool size), calculated from `src-range` / array length in config
- **Typical values**:
  - 1024 IPs uniform -> Hinf ~ 10.0
  - 512 IPs with one IP at 50% -> Hinf ~ 1.0
- **Tuning**:
  - IP pool is one of the largest entropy contributors
  - Low `min_src_ip` usually means some workers are not using the full IP pool
  - Check whether `interleave` and `batch` settings are restricting IP rotation

### 3. min_src_port / min_dst_port -- Port Diversity

- **Meaning**: Worst-case diversity of source/destination port distribution
- **Theoretical maximum**: log_2(port pool size), calculated from range in config, ceiling 16 bits
- **Typical values**: 100 ports uniform -> Hinf ~ 6.64
- **Tuning**:
  - Low port entropy usually means traffic is concentrated on few ports
  - For stateless testing, `min_dst_port` below expectation indicates insufficient port rotation

### 4. min_pkt_size -- Packet Size Diversity

- **Meaning**: Worst-case diversity of packet sizes
- **Theoretical maximum**: Depends on configured size range, ceiling 16 bits
- **Typical values**:
  - Fixed size -> Hinf ~ 0
  - 16 sizes uniform -> Hinf = 4.0
- **Tuning**:
  - Fixed packet size is suitable for performance baseline testing, but entropy testing needs multiple sizes
  - If Hinf is far below log_2(configured count), check for packet size sequence patterns (e.g., repeating the same order cyclically)

### 5. min_tcp_flags -- TCP Flags Diversity

- **Meaning**: Worst-case diversity of TCP flag combinations (SYN, ACK, PSH, FIN, RST, etc.)
- **Theoretical maximum**: 8 bits (max 256 combinations, but typically 4-8 in practice)
- **Typical values**:
  - Pure data flow (only PSH+ACK) -> Hinf ~ 0
  - Mixed SYN/ACK/DATA -> Hinf ~ 1.0-2.0
- **Tuning**:
  - Low Hinf in pure UDP scenarios is normal (no TCP flags)
  - In mixed TCP flow scenarios, low Hinf usually means handshake/teardown phases are missing

### 6. min_delta_tsc -- Inter-Packet Timing Diversity

- **Meaning**: Worst-case diversity of TSC intervals between adjacent packets
- **Theoretical maximum**: 64 bits (practically limited by time precision within the observation window); typical reference 4-12 bits
- **Typical values**:
  - Fixed interval -> Hinf ~ 0
  - Poisson model -> Hinf can reach 4-8 bits
  - Burst mode -> Hinf ~ 1-3 bits
- **Tuning**:
  - Fixed-interval transmission produces extremely low timing entropy, easily detected statistically
  - `traffic_model=1` (Poisson) can significantly increase timing entropy
  - In gateway testing, low timing entropy means send patterns are predictable, weakening anti-DDoS evaluation

### 7. min_outer_src_ip / min_outer_dst_ip -- VXLAN Outer IP Diversity

- **Meaning**: Worst-case diversity of VXLAN tunnel outer source/destination IPs
- **Theoretical maximum**: log_2(outer IP config count)
- **Typical values**: Determined by `vxlan.ether.type.ipv4.src/dst` array length in `conf/config.yaml`
- **Constraint**: **Sampled only on VXLAN-encapsulated packets**. If VXLAN ratio < 100%, actual sample count is low.
- **Tuning**:
  - If `min_outer_src_ip ~ 0` in VXLAN test scenarios, all VXLAN packets use the same outer source IP
  - Ensure multiple VTEP source addresses are configured

### 8. min_vni -- VNI Diversity

- **Meaning**: Worst-case diversity of VXLAN Network Identifiers
- **Theoretical maximum**: log_2(VNI config count), max 24 bits
- **Constraint**: **Sampled only on VXLAN-encapsulated packets**
- **Tuning**:
  - Multiple VNIs can simulate multi-tenant scenarios
  - If `min_vni` is low, check whether VNI is fixed

### 9. min_total_5tuple -- Aggregate Min-Entropy Sum

- **Meaning**: The **algebraic sum** of min-entropy across the following 7 dimensions:
  `protocol + src_ip + dst_ip + src_port + dst_port + pkt_size + tcp_flags`
- **Note**: This is addition, not joint entropy; it assumes independence among dimensions
- **Purpose**: A summary of the marginal dimensions. Automated checks should
  use its published window target rather than a fixed threshold.
- **Excludes**: delta_tsc, outer_src_ip, outer_dst_ip, vni -- these are conditional dimensions

---

## Tuning Strategy

### Example population targets

| Dimension | Recommendation | Notes |
|------|------|------|
| min_protocol | >= 1.0 bits | Multi-protocol environments |
| min_src_ip | >= 6.0 bits (64 IPs) | Core dimension |
| min_dst_ip | >= 3.0 bits (8 IPs) | Destination distribution |
| min_src_port | >= 5.0 bits (32 ports) | Source port entropy |
| min_dst_port | >= 3.0 bits (8 ports) | Destination port entropy |
| min_pkt_size | >= 2.0 bits (4 sizes) | When packet size varies |
| min_tcp_flags | >= 1.5 bits | TCP mixed flows |
| min_delta_tsc | >= 3.0 bits | Poisson model |
| min_total_5tuple | Sum of active component targets | Marginal aggregate, not joint entropy |

### Common Causes of Low Min-Entropy

1. **Configuration ranges too small**: Check range/array parameters in `config.yaml`
2. **Insufficient interleaving**: Packets cycle through IP/port pools in fixed order, causing short-term distribution skew -> increase `interleave-depth`
3. **Batch too large**: All packets in one burst share the same IP -> reduce `batch` or increase `batch_jitter_us`
4. **Single-core limitation**: Some dimensions are only effectively used on some lcores -> check port masks and queue assignments
5. **Improper protocol mix ratio**: E.g., 99% TCP + 1% UDP -> adjust `--tcp/--udp` weights
6. **VXLAN ratio too low**: Insufficient sample count for VXLAN dimensions -> increase `vxlan.ratio`

### Typical Diagnostic Flow

```
Detect min_total_5tuple below its window target
  +-> Check each dimension Hinf individually
      +-> A dimension ~ 0 -> that dimension has no variation (config issue)
      +-> A dimension < log_2(pool size) / 2 -> distribution heavily skewed
      |    +-> Check interleave + batch settings
      +-> A dimension near theoretical max -> normal, continue to next dimension
```

---

## Relationship with Shannon Entropy

Shannon entropy and min-entropy are displayed together and used in combination:

- **Shannon entropy ~ Min-entropy**: Distribution is relatively uniform; all values appear with similar frequency
- **Shannon entropy >> Min-entropy**: A few high-frequency values + many low-frequency values (long-tail distribution); `max p_i` is large but remaining values are dispersed
- **Shannon entropy < theoretical max**: Overall diversity is insufficient

```
Example: 100 values, 1 appears 50 times, the other 99 appear ~0.5 times each
  Shannon H = -[0.5*log_2(0.5) + 0.5/99*99*log_2(0.5/99)] ~ 3.5 bits
  Min-Entropy Hinf = -log_2(0.5) = 1.0 bit
  -> Gap of 2.5 bits indicates highly non-uniform distribution
```
