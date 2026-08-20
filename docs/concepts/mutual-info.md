# Mutual information

The entropy dashboard reports mutual information (MI) for selected pairs of
packet fields. MI measures dependence: it is zero when two sampled fields are
independent and rises as one field becomes more predictable from the other.

MI is descriptive, not a score. A low value is useful when a test needs
independent tuple selection. A non-zero value may be intentional when modeling
real traffic, such as different packet sizes for DNS and HTTP.

## Definition

For sampled variables `X` and `Y`:

```text
I(X;Y) = H(X) + H(Y) - H(X,Y)
```

The implementation packs each pair into a 64-bit key, sorts the keys, and
computes joint entropy from equal-key runs. Its range is:

```text
0 <= I(X;Y) <= min(H(X), H(Y))
```

A value near zero only means independence when both fields have enough samples
and non-trivial entropy. Two constant fields also produce zero MI.

## Reported pairs

| Tier | Pair | What it can reveal |
|------|------|--------------------|
| 1 | `I(Src IP; Dst IP)` | Fixed or restricted address pairings |
| 1 | `I(Src Port; Dst Port)` | Coupled source and destination port selection |
| 1 | `I(Protocol; Src Port)` | Protocol-specific source-port ranges |
| 2 | `I(Pkt Size; Dst Port)` | Service-specific packet sizes |
| 2 | `I(Pkt Size; Protocol)` | Protocol-specific packet sizes |
| 2 | `I(DeltaTSC; Protocol)` | Protocol-specific transmission timing |
| 2 | `I(DeltaTSC; Flow Key)` | Timing patterns tied to particular flows |
| 3 | `I(TCP Flags; Pkt Size)` | TCP flag and packet-size coupling |
| 3 | `I(TCP Flags; Src Port)` | Flag distributions tied to source ports |
| 3 | `I(TCP Flags; Dst Port)` | Flag distributions tied to destination ports |
| 4 | `I(Outer Src; Outer Dst)` | Fixed VXLAN endpoint pairings |
| 4 | `I(VNI; Outer Src)` | VNI-to-VTEP bindings |

Tier 3 uses TCP samples only. Tier 4 uses VXLAN samples only.

## Reading the tiers

### Tier 1: tuple selection

The first tier checks whether the generated 5-tuple dimensions vary
independently.

- `I(Src IP; Dst IP)` rises when sources are repeatedly paired with the same
  destinations. Check the configured IPv4 source and destination ranges.
- `I(Src Port; Dst Port)` rises when the source-port choice depends on the
  destination port. Check the TCP, UDP, and plugin port ranges.
- `I(Protocol; Src Port)` rises when protocols use distinct source-port ranges.
  This can be intentional in a realistic workload and undesirable in a hash
  distribution test.

For ECMP, RSS, NAT, or conntrack-distribution tests, compare these values with
the marginal entropies and the configured range sizes. MI alone does not show
whether the tuple space is large enough.

### Tier 2: size and timing

The second tier describes relationships between workload behavior and packet
classification.

- Packet-size MI reflects the shared IMIX configuration and any
  protocol-specific length constraints.
- `I(DeltaTSC; Protocol)` can expose protocol-dependent scheduling.
- `I(DeltaTSC; Flow Key)` can expose flows sent in clusters instead of being
  interleaved.

`DeltaTSC` is shifted right by 12 bits before packing. This reduces timestamp
resolution and keeps the number of distinct joint keys manageable.

Timing MI must be read with the Rate PSD and pacing configuration. A fixed-rate
stream can have low MI because every protocol sees the same fixed interval; it
does not follow that the timing is random.

### Tier 3: TCP behavior

These pairs use only packets for which TCP fields were collected. They show
whether flag distributions differ by packet size or port.

In `handshake` mode, flag-size dependence is expected: SYN and ACK packets do
not necessarily have the same length as data packets. Port dependence may point
to an uneven connection-state distribution or to an intentionally
service-specific workload.

### Tier 4: VXLAN

These pairs are populated only for VXLAN traffic.

- `I(Outer Src; Outer Dst)` describes dependence between tunnel endpoints.
- `I(VNI; Outer Src)` describes dependence between a VNI and its source VTEP.

High values may be correct for a topology that assigns each tenant to one VTEP.
For a many-to-many overlay test, they usually indicate that the configured
outer address or VNI combinations are too narrow.

## Marginal sample sets

Most pairs use the global marginal entropy values from the same snapshot. Two
groups require marginals derived from their joint arrays:

- `I(DeltaTSC; Flow Key)`, because only samples with a flow key participate;
- all Tier 3 pairs, because only TCP samples participate.

Deriving those marginals from the joint array keeps `H(X)`, `H(Y)`, and
`H(X,Y)` on the same population. Mixing global and filtered populations can
produce a biased result.

## Diagnostics

Before acting on an MI value, check:

1. The pair has enough samples. An empty or one-value population is not evidence
   of independence.
2. Both marginal entropies are non-zero. MI cannot reveal coupling when either
   field is constant.
3. The configured ranges permit the intended combinations.
4. The observation window covers several pacing cycles or connection
   lifetimes.
5. The dependence is unwanted for the test. Realistic workloads often contain
   deliberate size, port, and protocol relationships.

There is no universal good threshold. Compare runs made with the same sample
window and workload, or establish a baseline for the DUT test being performed.

## Related flow metrics

The snapshot also reports metrics that are not entries in the MI matrix:

| Metric | Population | Meaning |
|--------|------------|---------|
| `entropy_joint_5tuple` | sampled packets | Diversity of a compact 5-tuple key |
| `min_*` fields | sampled packets | Uncertainty of the most frequent value |
| `flow_entropy_5tuple` | handshake events | Diversity of tracked connections |
| `flow_entropy_event` | handshake events | Diversity of lifecycle events |
| `flow_entropy_lifetime` | completed flows | Diversity of connection lifetimes |
| `flow_distinct`, `flow_ratio` | tracked flows | Distinct-flow count and ratio |

Joint 5-tuple entropy measures diversity, while MI measures dependence between
two dimensions. Neither substitutes for the other.

## Implementation

The calculation is in `src/entropy_stats.c`. The common pattern is:

```c
uint64_t joint = ((uint64_t)upper << 32) | lower;

/* Sort joint keys and calculate H(upper, lower). */
qsort(keys, n, sizeof(keys[0]), cmp_u64);
double h_joint = shannon_from_sorted(keys, n, sizeof(keys[0]), cmp_u64, NULL);

double mi = h_upper + h_lower - h_joint;
```

For a filtered population, the marginal values come from the same joint keys:

```c
double h_upper = H_FIELD(keys, n, 1);
double h_lower = H_FIELD(keys, n, 0);
```

The joint arrays have fixed capacity. When adding a pair, update the snapshot
fields, collection and calculation code, JSON and Prometheus serialization,
dashboard, tests, and this table. Check stack use as part of the change.
