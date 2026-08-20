# Entropy Interpretation and Baselines

## Goal

The Entropy dashboard must answer three different questions separately:

1. **Measurement:** what did Bless observe?
2. **Expectation:** what should this workload configuration produce?
3. **Diagnosis:** how large is the difference and what should be changed first?

A raw entropy value cannot answer the last two questions. A value of 6 bits is
excellent for a 64-value pool and poor for a 65,536-value pool. Interpretation
therefore uses configuration-aware baselines rather than protocol field widths
or one global threshold.

## Common diagnostic model

Every metric that supports interpretation should expose the following logical
record, even if some fields are unavailable:

| Field | Meaning |
|---|---|
| `measured` | Value computed from the current observation window |
| `population_target` | Expected value for the configured population |
| `target` | 95% lower threshold attainable in the current sample window |
| `gap` | Direction-aware difference between target and measurement |
| `attainment` | Normalized progress toward the target |
| `evidence` | Sample count, distinct count and dominant count/probability |
| `baseline_source` | `configured`, `reference`, `observed-support`, or `unavailable` |
| `state` | `good`, `degraded`, `poor`, `informational`, `inactive`, or `insufficient-samples` |
| `action` | Dimension-specific next check or tuning operation |

Conditional dimensions must be `inactive` when their population is absent.
They must not be rendered as zero-quality measurements. Small populations must
be marked `insufficient-samples` instead of producing a confident diagnosis.
Dimensions without a configured population are marked `informational`. Their
observed-support target describes the current window, but is not a pass/fail
criterion for the generator.

## Min-entropy

Min-entropy is `Hinf(X) = -log2(max(p_i))`. It is the worst-case uncertainty of
one distribution, not the minimum of several entropy measurements over time.

For min-entropy the population target is:

```text
population_target = -log2(expected_max_probability)
target_hinf = finite_sample_lower_bound(population_target, samples, 95%)
gap_bits = max(0, target_hinf - measured_hinf)
attainment = measured_hinf / target_hinf
dominance_ratio = 2^gap_bits
```

`dominance_ratio` is the most actionable form of the gap. A 3-bit gap means
the observed dominant value is approximately eight times as frequent as the
configured baseline permits.

The population target must preserve configured weights. A 90/10 TCP/UDP mix has a target
of `-log2(0.9)`, not the 1-bit target of a uniform two-protocol mix. IMIX
duplicates likewise act as weights. Uniform IP and port pools use
`log2(pool_size)` only when every sampled packet participates. For unconditional
IP/port measurements, the configured target also includes the zero-value mass
from non-applicable protocols. Port baselines combine the separate TCP, UDP,
SCTP and extension pools by protocol weight instead of selecting one range. Multi-
protocol IMIX is reported against observed support until different L3/L4 header
lengths can be composed into an exact configured packet-size distribution.
Timing and stateful TCP-flag targets likewise remain observed-support baselines
until their traffic-model distributions can be represented faithfully.

`min_total_5tuple` remains an algebraic diagnostic sum, not joint entropy. A
sum containing observed-support components is explicitly labelled `mixed`; it
is useful for within-run diagnosis but must not be presented as configured
workload compliance.

## Applying the model to the rest of the page

The same record applies with metric-specific gap direction:

- **Shannon entropy:** compare with configuration-weighted Shannon entropy;
  report effective diversity (`2^H`) as well as bit gap.
- **Mutual information:** the desired value depends on intent. Independence
  tests target zero, while affinity tests need a configured or recorded
  reference. Never label all high MI as bad.
- **Flow metrics:** compare distinct-flow ratio and churn with workload goals;
  retain packet and flow counts as evidence.
- **Rate PSD:** retain its domain interpretation, but add reference bands for
  dominant frequency and spectral flatness when a known-good run exists.
- **Handshake and latency:** use SLO targets and sample counts; distinguish an
  inactive workload from a measured zero.

The UI should render one ranked Interpretation table rather than independent
hard-coded color rules. Ranking uses severity first, then normalized gap, so
the largest actionable deviation appears first.

As an initial application beyond min-entropy, Shannon IP, port and VXLAN bars
use their configured pool ceilings rather than the raw 32/16/24-bit field
widths. Weighted Shannon targets remain a later step because their expectation
must preserve protocol and IMIX weights.

## Delivery order

1. Compute min-entropy configured targets and observation evidence in the
   statistics path.
2. Publish measured/target/gap/attainment/dominance/state through JSON and
   Prometheus.
3. Add a dynamic, gap-ranked Min-Entropy Interpretation table.
4. Add inactive and insufficient-sample semantics for conditional dimensions.
5. Reuse the record for Shannon, MI, flow, PSD, handshake and latency metrics,
   adding reference-run persistence only after configured baselines are stable.
