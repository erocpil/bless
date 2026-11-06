# Entropy Theory -- Statistical Foundations of BLESS

> Version 3.0.
> This document describes the theoretical model underlying BLESS's
> entropy-controlled traffic generation.  It is self-contained and does not
> reference source code.  For implementation details, see the Architecture &
> Design overview.

---

## 1. Why Measure Entropy

Traditional network stress-testing tools measure throughput -- maximum PPS,
maximum Mbps, minimum latency.  These metrics answer "how much traffic can the
gateway handle," but they cannot answer a more fundamental question:

**"How closely does the test traffic resemble real-world traffic?"**

A generator sending identical packets in a fixed pattern can saturate the line
rate.  But the gateway under test may trigger hardware pattern caches,
flow-table hit optimizations, and interrupt coalescing — effects that can
produce throughput measurements above what the same gateway would achieve under
diverse, variable traffic.  When the same gateway faces genuine Internet
traffic, the 5-tuple of each flow, packet size distribution, and inter-arrival
gaps typically show higher entropy than fixed-pattern traffic: lower cache hit
rates, higher state-table pressure, and more frequent interrupts.

BLESS's core methodology: **quantify traffic randomness as measurable entropy
metrics, and treat entropy as a first-class citizen in stress testing -- on
equal footing with PPS and latency.**

---

## 2. Shannon Entropy

### 2.1 Definition

For a discrete random variable X with value set {x_1, ..., x_n} and probability
distribution P(X=x_i)=p_i, Shannon entropy is:

```
H(X) = -SUM p_i log_2(p_i)
```

With the convention 0*log_2(0) = 0.  Entropy is measured in bits.

Intuition: H(X) is the minimum encoding length needed to describe X.  H(X)=0
means X is fully determined (a degenerate distribution with p=1); H(X)=log_2(n)
means X is uniformly distributed over n values -- the theoretical maximum.

### 2.2 Entropy Dimensions in a Traffic Generator

A packet stream carries entropy across multiple orthogonal dimensions:

| Dimension | Variable | Max Theoretical | What It Measures |
|-----------|----------|----------------|------------------|
| Protocol | protocol | log_2(N_proto) | Uniformity of protocol type distribution |
| Source IP | src_ip | log_2(N_src_ip) | Utilization of source address space |
| Destination IP | dst_ip | log_2(N_dst_ip) | Utilization of destination address space |
| Source Port | src_port | log_2(N_src_port) | Source port diversity |
| Destination Port | dst_port | log_2(N_dst_port) | Destination port diversity |
| Packet Size | pkt_size | log_2(N_sizes) | Uniformity of packet size distribution |
| TCP Flags | tcp_flags | log_2(N_flags) | Diversity of TCP flag combinations |
| Inter-arrival Gap | delta_tsc | -- | Temporal randomness (continuous, no natural bound) |
| Joint 5-tuple | joint_5tuple | SUM of component H | Composite randomness of flow identity |
| VXLAN Presence | vxlan_encap | <= 1.0 | Tunnel vs. non-tunnel mix |
| Outer Source IP | outer_src_ip | log_2(N_outer_ip) | VXLAN outer IP diversity |
| VNI | vni | log_2(N_vni) | VXLAN Network Identifier diversity |

A fixed-pattern generator may have only one or two flows (`H` near 1). A larger
joint entropy means the gateway must track a wider set of connections. Compare
the result with the configured tuple space; Internet traces are not a useful
baseline unless the experiment is intended to reproduce that trace.

---

## 3. Min-entropy and Mutual Information

### 3.1 Min-entropy: Worst-Case Measurement

```
Hinf(X) = -log_2(max(p_i))
```

Min-entropy concerns only the most probable value.  It answers: "What is the
probability that an adversary can guess X in a single attempt?"

Shannon entropy is an *average* measure -- a distribution can appear random
overall while one value occurs disproportionately often:

| Distribution | H | Hinf |
|-------------|----|------|
| 64 values, uniform | 6.00 | 6.00 |
| 63 values at 1%, 1 value at 37% | ~5.5 | 1.43 |

In stress testing, Shannon entropy may be inflated by many low-frequency
values, while the actual pressure on the gateway comes from the high-frequency
ones -- the flow-table entries hit most often.  When the gap between H and Hinf
is significant, the configuration has "hotspot values" that need widening.

### 3.2 Mutual Information: Dimensional Independence

```
I(X;Y) = H(X) + H(Y) - H(X,Y)
```

Mutual information measures how much one variable predicts another.  I(X;Y)=0
means independence; I(X;Y)=min(H(X), H(Y)) means one dimension fully determines
the other.

In a traffic generator, dimensions are naturally coupled:

| Coupling | Example | I |
|----------|---------|---|
| Intentional | TCP ports only exist for TCP protocol | I(proto; src_port) > 0 |
| Construction artifact | IP range too small causes IP-port correlation | I(src_ip; src_port) > 0 |
| Temporal | Fixed inter-packet gap couples timing with flow identity | I(delta_tsc; flow_key) > 0 |

**High mutual information means the traffic has "patterns."**  Each dimension
may individually show normal entropy, but the joint distribution reveals
predictable structure.  Traditional PPS-based stress testing ignores this
entirely.

BLESS tracks 12 MI pairs, including I(proto; src_ip), I(src_ip; dst_ip),
I(pkt_size; proto), I(delta_tsc; flow_key), and I(outer_src_ip; VNI).  All MI
is computed via a single-pass joint-key sort, guaranteeing numerical stability
(I <= min(H(X), H(Y))).

---

## 4. Controlled Entropy: The Three-Axis Model

Entropy in a packet stream is not a single number.  It is shaped by three
independent control axes, each with distinct mechanisms and bounds.

### 4.1 Axis 1: Packet Morphology

Every field in a packet is an entropy source.  Control is layered:

**L1 -- Distribution.**  The IP ranges, port ranges, and protocol weights
determine the *theoretical ceiling* of each dimension's entropy.  A source IP
range of `172.16.0.0/12` implies H_max(src_ip) = 20 bits.  Whether that ceiling
is reached depends on the other axes.

**L2 -- Construction.**  Different protocols carry entropy in different fields:
UDP carries it in ports and payload; TCP adds flags and sequence numbers; ICMP
adds type and code; VXLAN adds outer IP/UDP and VNI.  The choice of protocol
weights determines *which fields* contribute to the total entropy budget.

**L3 -- Perturbation.**  On top of normal structure, field-level anomalies
(corrupt checksums, invalid flags, truncated lengths) inject additional
unpredictability.  These perturbations are applied at a controlled ratio,
independent of the L1/L2 layers.

### 4.2 Axis 2: Temporal Behavior

The inter-packet timing distribution determines H(delta_tsc).  This is a
spectrum:

```
Deterministic ---> Uniform jitter ---> Poisson ---> Pareto ON-OFF
(H -> 0)                                                     (H high)
```

- **Deterministic:**  Fixed interval between bursts.  H(delta_tsc) ~ 0.  The
  gateway sees a perfectly regular pulse.  Measures peak throughput.
- **Uniform jitter:**  +/- N microseconds around the base delay.  H > 0.
  Simulates light traffic fluctuation.
- **Poisson:**  Exponentially-distributed inter-arrival times.  H significantly
  higher.  Simulates real network traffic with memoryless arrivals.
- **Pareto ON-OFF:**  Heavy-tailed burst durations alternating with long idle
  periods.  Produces self-similar traffic with Hurst parameter > 0.5, matching
  observed Internet traffic characteristics.

Rate control (token bucket) applies uniformly across all timing models,
constraining the *volume* without affecting the *distribution shape*.

### 4.3 Axis 3: Measurement and Feedback

The three axes form a closed loop:

```
Morphology (L1/L2/L3)  -- determines theoretical ceiling -->
    Temporal Behavior   -- determines actual utilization -->
        Measurement     -- quantifies gap -->
            Feedback    -- adjusts morphology or timing -->
```

| Element | What it controls | Metric |
|---------|-----------------|--------|
| Morphology | Per-packet information content | H(protocol), H(src_ip), ... |
| Temporal | Inter-packet randomness | H(delta_tsc) |
| MI | Cross-dimension coupling | I(X;Y) |
| Feedback | Adaptive rate adjustment | PPS_new = PPS + K_p*(H_target - H) |

**Example interplay:** A uniform pool of 2^20 source addresses has a population
ceiling of 20 bits. A finite window may contain far fewer distinct addresses.
Burst timing and source/destination pairing are separate properties: tightly
spaced packets do not by themselves imply high address MI, and changing ports
cannot directly remove dependence between source and destination IPs.
Measure each property before choosing a control to change.

### 4.4 The Three Axes as a Design Space

The three-axis model turns traffic generation from a one-dimensional
throughput problem into a multi-dimensional design space.  An operator selects:

1. **What fields carry entropy** (morphology)
2. **When packets arrive** (temporal)
3. **What the feedback target is** (measurement)

These constraints define an experimental target; convergence and attainability
must be measured rather than assumed.

---

## 5. Sampling and Measurement

### 5.1 The Sampling Problem

Entropy is a population statistic, estimated from a finite packet sample.
The estimator sorts selected values, counts equal-value runs and computes
`H = -SUM (run_length/N) * log2(run_length/N)`.
Sorting costs O(N log N); the subsequent scan costs O(N).

Selection policy and retained-window capacity both matter.
Sampling a small subset or overwriting unread samples changes the evidence
available for the estimator, even if traffic generation itself continues normally.

### 5.2 Sample Size and Accuracy

Accuracy depends on distribution shape, support size, dependence between samples
and observation length. There is no universal standard-error bound based only
on N and the number of observed values that validates all these workloads.
Empirical entropy cannot exceed `log2(N)` for N observed samples, so a small
window cannot demonstrate coverage of an arbitrarily large configured population.
Use finite-sample baselines and report evidence alongside the estimate.

### 5.3 Off-Path Computation

Separating sampling from sorting avoids running the full statistical calculation
inside each packet constructor. It does not remove the cost of collecting samples,
cache traffic or aggregate computation. Capacity and measurement overhead need
validation at the intended rate, worker count and observation interval.

---

## 6. Adaptive Control: The P Controller

### 6.1 Design Choice

The proportional-error rate update can be idealized as:

```text
PPS[k+1] = PPS[k] + K_p * (H_target - H[k])
```

This is an incremental rate update; the rate carries state between iterations.
It has no separate accumulated-error or derivative term.
Keeping the feedback simple avoids introducing derivative noise amplification
and additional saturation handling before the workload response is characterized.
It does not establish an acceptable universal entropy error or guarantee stability.

### 6.2 Conditional Stability Model

Assume a delay-free local response `H = f(PPS)` with positive slope `a` near an
attainable equilibrium. Ignoring quantization and saturation, a perturbation obeys:

```text
delta_PPS[k+1] = (1 - a*K_p) * delta_PPS[k]
```

Under those assumptions, `0 < a*K_p < 2` is the local linear stability condition.
This is not a measured model of every Bless workload. Sampling, delayed updates,
saturation, integer truncation and a zero value that means unlimited rate can
invalidate it. A zero or negative slope does not satisfy the positive-slope model.
No default gain is derived from this equation in the implementation.

The actual update direction, limits and concurrency policy are documented in
[the architecture](../overview.md#33-p-controller-why-p-instead-of-pid).

---

## 7. PRNG Requirements

### 7.1 State, Outputs and Range Mapping

A PRNG's output range does not determine its state period.
The C library interface alone does not specify a particular algorithm, so neither
an LCG implementation nor a repetition period can be inferred from `RAND_MAX`.
For traffic generation, evaluate the actual output bits and range reduction used
for fields, as well as the state transition.

### 7.2 Evaluation Criteria

- Sufficient state period for the number of random draws, not just packet count.
- Reproducible initialization with fixed topology and call order.
- No shared-state contention between workers.
- Statistical tests for the actual output and field-mapping path.
- Separate treatment of cryptographic unpredictability, which is not established
  by ordinary distribution tests or thread-local state.

Algorithm-family results do not substitute for measurements of a particular
implementation. The repository does not provide BigCrush/PractRand results for
its complete output/range-mapping path.

### 7.3 Multi-Dimensional Independence

Separate draws from a deterministic stream do not prove independence.
Workload constraints, reused values, range mapping, sampling and the PRNG itself
are all candidates when investigating measured dependence.
Use reproducible experiments before selecting a different generator or separate
state per dimension; a universal MI threshold cannot identify the cause.

---

## 8. Known Limitations

### 8.1 Second-Order Temporal Moments

Current H(delta_tsc) measures first-order uniformity of the gap distribution.
It does not capture autocorrelation of inter-arrival gaps.  Real Internet
traffic exhibits long-range dependence (Hurst parameter > 0.5, self-similarity
across timescales).  The Pareto ON-OFF model partially addresses this at the
generation level, but BLESS does not yet measure second-order statistics in
real time.

### 8.2 Spatial Locality and RSS

Interleaving changes temporal order within a batch and can reduce consecutive
same-flow accesses. It does not change the flow hash or remove RSS queue affinity.
Compare DUT cache and queue behavior directly instead of inferring it from the
presence of a shuffle alone.

### 8.3 Application-Layer Entropy

Current measurements cover L2-L4 headers plus packet size and timing.
Application-layer entropy (DNS query names, HTTP Host headers, NTP timestamps,
TLS SNI) is generated but not yet measured as formal entropy dimensions.
These are significant for DPI gateway testing.

### 8.4 Continuous Dimensions

Inter-arrival gap is a continuous variable.  Shannon entropy is defined for
discrete distributions.  BLESS discretizes delta_tsc into log-scale buckets,
which introduces quantization error.  Differential entropy (the continuous
analogue) would be more appropriate but is less interpretable in practice.

---

## 9. Comparison with Other Tools

| Dimension | BLESS | TRex | pktgen | Moongen |
|-----------|-------|------|--------|---------|
| Entropy measurement | 13-dim H + 12-dim Hinf + 12-pair MI | None | None | None |
| Adaptive closed-loop | P controller | None | None | None |
| Anomaly injection | 44+ categories | Limited | Limited | None |
| Flow-level analysis | 5-tuple entropy + connection lifecycle | Yes | No | No |
| Deployment | Single static binary | Python + scapy | Single binary | Single binary |

---

## 10. References

1. Shannon, C. E. (1948). "A Mathematical Theory of Communication." Bell
   System Technical Journal.
2. Vigna, S. (2016). "An experimental exploration of Marsaglia's xorshift
   generators, scrambled." ACM Transactions on Mathematical Software.
3. L'Ecuyer, P. & Simard, R. (2007). "TestU01: A C library for empirical
   testing of random number generators." ACM Transactions on Mathematical
   Software.
4. Cover, T. M. & Thomas, J. A. (2006). *Elements of Information Theory*.
   Wiley.
5. Paxson, V. & Floyd, S. (1995). "Wide area traffic: the failure of Poisson
   modeling." IEEE/ACM Transactions on Networking.

---

*For implementation details -- how these theoretical models map to DPDK
workers, configuration parameters, and the extension API -- see the
Architecture & Design overview (`docs/overview.md`).*
