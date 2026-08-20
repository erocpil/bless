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

**Example interplay:**  A source IP range of /12 sets H_max = 20 bits.  But if
packets are sent in tightly-spaced deterministic bursts, the gateway's conntrack
sees highly correlated 5-tuples within each burst.  I(src_ip; dst_ip) spikes,
and effective entropy is far below 20 bits.  Fix: either reduce burst density
(axis 2) or inject port-level perturbations (axis 1, L3) to decorrelate.

### 4.4 The Three Axes as a Design Space

The three-axis model turns traffic generation from a one-dimensional
throughput problem into a multi-dimensional design space.  An operator selects:

1. **What fields carry entropy** (morphology)
2. **When packets arrive** (temporal)
3. **What the feedback target is** (measurement)

The system then converges on the intersection of these constraints.  This is
analogous to how a PID controller converges on a setpoint -- but in an
information-theoretic rather than physical domain.

---

## 5. Sampling and Measurement

### 5.1 The Sampling Problem

Entropy is a population statistic.  In a live system generating millions of
packets per second, computing exact H over every packet is infeasible.  The
solution is periodic sampling:

1. Sample N packets per statistics interval (N <= 32,768)
2. Sort sampled values: O(N log N)
3. Single scan to count run lengths
4. Compute H = -SUM (run_len/N) * log_2(run_len/N)

### 5.2 Sample Size and Accuracy

For a dimension with k distinct values, the standard error of the empirical
entropy estimator is approximately sqrt((k-1)/(2N)).  With N=32,768 and k up to
256, the standard error is < 0.06 bits.  This is well below the measurement
noise floor introduced by traffic fluctuations within the sampling interval.

### 5.3 Off-Path Computation

Sorting O(N log N) on the data plane would introduce jitter.  The solution:
decouple sampling from computation.  Workers on the data plane write samples to
lock-free ring buffers (single atomic write per sample).  A control-plane
processor drains the buffers and runs the sorts during idle periods.  The
control-plane idle time exceeds 99%, so entropy computation is kept off the
worker fast path and was not measurable in the reported benchmark
environment.

---

## 6. Adaptive Control: The P Controller

### 6.1 Design Choice

BLESS uses a pure proportional (P) controller to adjust packet rate toward a
target entropy:

```
PPS_new = PPS_current + K_p * (H_target - H_current)
```

**Why not PID?**

- **Stress testing tolerates steady-state error.**  +/-5-10% entropy deviation
  does not affect stress-test validity.  Eliminating it with an integral term
  is unnecessary.
- **The derivative term amplifies measurement noise.**  Entropy estimates have
  inherent variance (quantization, Poisson sampling fluctuations, nonlinear
  gateway behavior).  A D term would produce unnecessary rate jitter.
- **The integral term risks windup.**  If the gateway drops all packets, rate
  clamps to zero but the integral accumulates.  On recovery, the I term releases
  a surge, causing overshoot.  Safe I requires anti-windup logic -- complexity
  that does not match the benefit.

### 6.2 Stability Analysis

Model the system in discrete time.  Let H = f(PPS) -- entropy is a function of
packet rate (higher rate means more samples, bringing H closer to its
theoretical maximum within each interval).  Linearizing around equilibrium:
H ~ a * PPS.

```
PPS[k+1] = PPS[k] + K_p * (H_target - a * PPS[k])
        = (1 - a*K_p) * PPS[k] + K_p * H_target
```

Stability condition: |1 - a*K_p| < 1, i.e. 0 < K_p < 2/a.

In practice, a is small (each packet contributes ~1/N to the sample set, so
H changes slowly with PPS).  This gives a wide stability margin.  BLESS
defaults to K_p = PPS_MAX / H_target, keeping the system well within the stable
region.

---

## 7. PRNG Requirements

### 7.1 The Problem with Standard rand()

The C standard library `rand()` is a linear congruential generator (LCG) with
RAND_MAX typically 2^31-1.  Two fatal issues for traffic generation:

1. **Period too short.**  At 10 Mpps the sequence repeats after ~200 seconds.
   Packets generated 200 seconds apart share the same PRNG state, introducing
   deterministic structure into what should be random traffic.
2. **Low-bit periodicity.**  LCGs exhibit short periods in lower-order bits.
   Port numbers derived from `rand() % 65536` do not uniformly explore the port
   space.

### 7.2 Requirements for Traffic-Grade PRNG

| Requirement | Why |
|-------------|-----|
| Period >> total packets in test duration | No cyclic structure detectable in any practical run |
| Full-bit diffusion | Low-order bits (ports, flags) as random as high-order bits (IPs) |
| Per-core state independence | No sharing between parallel workers (cache-line contention) |
| Seed reproducibility | Same seed produces identical packet sequences (deterministic replay) |
| Statistical test suite pass | BigCrush, PractRand (industry standard) |

xorshift64* (Vigna, 2016) satisfies all five.  Period 2^64-1.  Three xorshift
operations per output.  Passes BigCrush and PractRand.  Per-core state via
thread-local storage with seed derived from TSC XOR thread ID -- or an explicit
user-provided seed for deterministic replay.

### 7.3 Multi-Dimensional Independence

A common concern: if all dimensions draw from the same PRNG stream, can
correlated outputs create spurious Mutual Information?  With the current
generator design, substantial measured mutual information is expected to arise
primarily from workload configuration, protocol constraints, timing behavior,
or finite-sample effects rather than from the PRNG alone.

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

BLESS's interleave mechanism breaks spatial clustering of same-flow packets by
randomizing transmission order.  However, real NIC RSS distribution preserves
flow locality -- same-flow packets land on the same queue.  Full interleave
breaks this entirely, potentially underestimating cache-friendliness in
scenarios where RSS-based affinity matters.

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
