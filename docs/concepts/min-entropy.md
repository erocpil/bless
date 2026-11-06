# Min-Entropy: Meaning and Tuning Guide

## What Is Min-Entropy

For the most frequent value's probability `p_max`:

```text
Hinf(X) = -log2(p_max)
```

This measures the uncertainty of the most likely outcome of one distribution.
It is not the minimum of entropy readings over time, nor proof of cryptographic
unpredictability of a deterministic traffic generator.
For a uniform population of N values it is `log2(N)`; a value with probability
one half gives 1 bit, regardless of how many other values are possible.

A finite observation window estimates probabilities from counts.
Even a uniform generator can produce a repeated dominant value in a small
window, so compare against the published finite-sample target rather than
requiring exact equality with the population entropy.
[Entropy Interpretation and Baselines](entropy-interpretation.md) defines
`population_target`, the window `target`, sample evidence and diagnostic states.

## Per-Dimension Guide

| Dimension | What to inspect | Interpretation boundary |
|---|---|---|
| `min_protocol` | Configured protocol weights | A deliberate 90/10 mix has population target `-log2(0.9)`, about 0.152 bits, not 1 bit |
| `min_src_ip`, `min_dst_ip` | Address pools, weights and protocols participating in the measurement | Pool size alone is insufficient when non-applicable packets contribute a zero value |
| `min_src_port`, `min_dst_port` | Each transport/plugin port pool and its weight | A fixed service port can correctly have zero entropy |
| `min_pkt_size` | IMIX weights, repeated entries and protocol header lengths | Mixed-protocol lengths may use observed-support rather than configured baselines |
| `min_tcp_flags` | TCP population and connection state | Absent TCP is inactive; stateful flags do not have a universal uniform target |
| `min_delta_tsc` | Sampling, burst timing and pacing model | Locally measured TX timing, not NIC departure timing or a universal Poisson threshold |
| `min_outer_src_ip`, `min_outer_dst_ip`, `min_vni` | VXLAN subset and intended VTEP/VNI topology | Absent VXLAN is inactive; a small subset may be insufficient for diagnosis |
| `min_total_5tuple` | Individual component results and baseline sources | Algebraic sum of protocol, inner IP/port, size and flag min-entropies; not measured joint entropy |

A sum of marginal entropies does not establish independence.
Use [mutual information](mutual-info.md) to investigate pairwise dependence;
even all measured pairs cannot prove full multivariate independence.

## Tuning Strategy

### Example population targets

These are mathematical examples, not recommended acceptance thresholds.
They assume the stated population and weights; the window target is usually lower.

| Population | Population Hinf | Explanation |
|---|---|---|
| One fixed destination port | 0 bits | Correct for a single-service test |
| 64 equally likely source IPs | 6 bits | Only if all measured packets use that pool |
| 32 equally likely source ports | 5 bits | Only if weights and participating protocols preserve that distribution |
| Four equally likely packet sizes | 2 bits | Repeated IMIX entries or differing headers change the distribution |
| Two protocols weighted 90/10 | About 0.152 bits | The larger weight determines `p_max` |

### Diagnostic sequence

1. Identify the intended population and configured weights.
2. Check `inactive` and `insufficient-samples` before interpreting a low value.
3. Inspect sample count, distinct count and dominant probability; check sampler
   overwrites and the window target.
4. For a configured baseline, compare measured entropy with that target.
   Treat observed-support baselines as descriptive rather than workload compliance.
5. Check whether the dominant value matches an intended fixed service, weight or
   tenant binding before changing the configuration.
6. Change one relevant pool or weight, repeat with the same sampling conditions,
   and compare the evidence.

Batch shuffling changes ordering, not the histogram of a complete batch.
Increasing `interleave-depth` cannot by itself create missing IPs or ports or
repair an intentionally skewed population; it can affect what a finite sampling
window observes. Large batches also do not imply that all packets share one IP.
Use the [Chinese scenario guide](../zh-CN/guides/scenarios.md) for worked
conntrack, timing and VXLAN interpretations.

## Relationship with Shannon Entropy

For the same probability distribution, `Hinf <= H`.
A large difference indicates a dominant value with diversity in the remaining
population; whether that is desirable depends on the workload.

For a population of 100 values with probability 0.5 on one value and the other
0.5 shared equally by 99 values:

```text
H = 1 + 0.5 * log2(99) = approximately 4.315 bits
Hinf = -log2(0.5) = 1 bit
```

These are population probabilities, not fractional observed counts.
The gap is about 3.315 bits; it is not a universal pass/fail threshold.
