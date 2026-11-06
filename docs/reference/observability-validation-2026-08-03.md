# Observability Validation: 2026-08-03

## Scope

This note records the live validation of the entropy dashboard and statistics
API after deploying commit `1a1db82a74ba1c418a49629ef83aad5640c9085b` with
`conf/config-test.yaml`. The effective settings were traffic model 0, batch 64,
1,000 microseconds between batches, 5 microseconds of jitter, and an entropy
sample interval of 10.

## Results

The generator sustained about 64.2 Kpps without port errors or sampler
overwrites. Submission and burst percentiles were ordered, the legacy
enabled-lcore-normalized CPU metric was about 20%, and resident memory was
about 25 MiB.  That legacy percentage did not record its normalization
denominator and is not a per-worker PMD utilization measurement.  The original
run did not retain raw CPU samples, so CPU conclusions come from the repeat
validation below.

All dimensions with a configuration-derived min-entropy baseline reported
`good`. In particular:

- the TCP/UDP/SCTP source-port population no longer had the former SCTP-sized
  concentration; the largest sampled source-port probability was about 1.15%;
- VXLAN targets correctly represented two outer source addresses, three outer
  destination addresses, and five VNIs;
- packet size, TCP flags, timing, and the mixed aggregate used
  `informational` when no complete configuration-derived baseline existed;
- sampler overwrite counters remained zero;
- the HyperLogLog estimate was bounded by the sampled-flow total, so the API
  preserved `flow_distinct <= flow_total` and `flow_ratio <= 1`.

The rate spectrum contained peaks near 1, 2, 3, and 4 kHz. This is expected for
narrow batches repeated approximately every millisecond: the repetition rate
is the fundamental and the sharper pulse shape produces integer harmonics. A
strongest spectral peak at about 2 kHz therefore does not imply a 2 kHz pacing
period.

Remote probes from one routed environment intermittently received TCP
connection refusals while the target host showed the expected process, commit,
executable, and `0.0.0.0:8000` listener. Local API requests remained successful.
The available evidence did not show a Bless process or listener exit. The
WebSocket broadcast lifetime race found during that investigation was fixed in
`1a1db82`; routed connectivity remains an environment-level observation unless
server-side evidence shows otherwise.

## CPU Revalidation: 2026-08-15

A follow-up measurement (commit `fd51158`) repeated the check with
`tools/observability_cpu_validation.py`, which derives CPU cost from per-thread
`/proc` CPU time rather than the dashboard `bless_cpu_busy_pct`.

**Environment and method.** Four scenarios — 1 ms pacing and unlimited, each
with the entropy sampler disabled and with `sample-interval=10` — were
interleaved and each run in a fresh process five times (5 s warmup, 20 s
measurement per run). The binary was `build/release-static/bin/bless` with
`conf/config-test.yaml`, whose `vdev: net_pcap0,iface=lo` uses the pcap PMD on
the loopback interface (the file header comment still says `net_null0`). One TX
worker plus the main lcore were active; the EAL detected 40 host CPUs.

**Results (five restarts per scenario).** At the time of this measurement
(commit `fd51158`, before the main-lcore busy-wait fix) the process used about
2.0 CPU cores in every scenario, split evenly between the TX worker and the main
control lcore:

- `process_cpu_cores_used` between-run P50 was 1.999 in all four scenarios
  (min 1.998, max 2.000 across the 20 runs).
- Per-thread: the `tx_only@1` worker busy-polled at about 1.0 core and the main
  `bless` thread at about 1.0 core; all other enabled lcores and civetweb
  threads were idle.
- The dashboard `bless_cpu_busy_pct` reported about 20%. This is a
  per-enabled-lcore normalization, not spare CPU capacity.

**Subsequent corrections.** Two later fixes changed both the numerator and the
process CPU profile:

- The main control lcore was busy-waiting (`rte_delay_ms`) until commit
  `c4d17a4` replaced it with an absolute-time `clock_nanosleep`.  After that the
  process drops to about 1.0 core (the single busy-polling TX worker; the main
  lcore is below 0.01 core), so the "2.0 cores" figures above are historical.
- The `bless_cpu_busy_pct` numerator originally used `CLOCK_PROCESS_CPUTIME_ID`.
  A diagnostic (`ddea0c1`) showed the divisor is `rte_lcore_count()=5`, and a
  later root-cause investigation found that on this host (`isolcpus=0-16`,
  `nohz_full=0-16`) the kernel batches per-thread CPU accounting, so sub-second
  `CLOCK_PROCESS_CPUTIME_ID` deltas miss the busy-polling worker lcore threads
  (they do not *omit* the threads in a long window, but short-window sampling
  misses them).  The numerator was replaced with a per-thread
  `/proc/self/task/*/stat` `utime+stime` sum over a ≥1 s window, which matches
  the external validation script's accounting.  With one busy-polling worker and
  five enabled lcores the metric now correctly reads about 20% (≈1.0 core ÷ 5).

- Throughput: about 0.064 MPPS paced and about 0.43 MPPS unlimited.
- Both sampler settings occupied the same two busy-polling cores.  That does
  not demonstrate zero sampler cost: the throughput P50 changed by about
  -0.12% when paced and -0.27% when unlimited, below the observed between-run
  variation.  In the unlimited scenario the sampler also produced about 1,000
  overwrites per 20 s window, so the entropy window was incomplete there.

## Remaining Work

Repeat the CPU portion after deploying the explicit process-core metrics.  The
updated `tools/observability_cpu_validation.py` records those metrics alongside
an independent sum of per-thread `/proc` CPU time, allowing the reported
normalization denominator and process-core value to be cross-validated.

Immediate observability work:

- publish the fundamental rate and strongest spectral peak separately;
- avoid dumping request headers and connection metadata for every statistics
  request;
- keep the Phase 1 status and API documentation aligned with these changes.

Remaining Phase 1 software verification:

- complete non-loopback, key-rotation, and observation-only control-plane
  integration cases;
- stress multiple HTTP and WebSocket clients, slow readers, and repeated
  disconnects under sanitizers;
- exercise maximum synthetic port and xstats serialization boundaries.

Hardware validation remains necessary for authenticated runtime changes on
active packet workers, the Reference Ladder throughput procedure, handshake RX
and latency behavior through a DUT, and wire-time pacing accuracy.

Longer-term work includes hardware pacing backends, multi-field atomic runtime
updates, flow mode, optional tunnel and impairment features, configuration
parser modularization, and release hardening.
