# Rate PSD -- Frequency-Domain Traffic Analysis

Bless's Rate PSD (Power Spectral Density) module performs **frequency-domain analysis**
of the TX packet rate, detecting periodic structure that signal-domain (Shannon)
and information-domain (MI) metrics cannot see.

This is the **beta metric** of Bless's frequency-domain observability pipeline.
It answers: *"Is the TX rate smooth and aperiodic (high entropy), or does it contain
hidden rhythmic structure (DUT rate-limiters, token-bucket cycles, traffic-construction
artefacts)?"*

---

## Quick Reference

| Field | JSON Key | Unit | Range | Meaning |
|-------|----------|------|-------|---------|
| Fundamental Frequency | `psd.fundamental_hz` | Hz | 0-5000 | Lowest significant peak supported by another peak in its harmonic series; falls back to the strongest peak |
| Strongest Peak | `psd.strongest_peak_hz` | Hz | 0-5000 | Frequency bin with the highest spectral power |
| Dominant Frequency | `psd.dominant_hz` | Hz | 0-5000 | Compatibility alias for `strongest_peak_hz` |
| Spectral Flatness | `psd.spectral_flatness` | dimensionless | 0-1 | Wiener entropy: 0 = pure tone (periodic), 1 = white noise (aperiodic) |
| Signal Valid | `psd.signal_valid` | boolean | true/false | Whether the window contains measurable non-DC power |
| PSD Bins | `psd.bins[256]` | power | >= 0 | Power per frequency bin (0-Nyquist); index x 39.0625 Hz = frequency |

> Detailed analysis, diagnostic guidance, and configuration recommendations for each
> metric are in the sections below.

---

## 1. Design Rationale

### 1.1 The Blind Spot of Shannon Entropy

Shannon entropy on inter-packet timing (`entropy.delta_tsc`) measures **distribution
uniformity** -- whether all inter-packet gaps occur with equal probability.  It does
not capture **ordering**: the same set of gap values arranged differently in time
produce identical Shannon entropy.

Two traffic streams with identical `H(delta_tsc)` can have completely different
frequency-domain signatures:

| Stream | H(delta_tsc) | Dominant Freq | Real-World Cause |
|--------|-------------|---------------|------------------|
| Uniform random gaps | 6 bits | none (flat) | `traffic_model=1` (Poisson) |
| 1 ms gap, 6 ms gap, 1 ms gap, 6 ms gap... | 6 bits | 125 Hz | DUT token-bucket refill at 8 ms period |
| All gaps = 2 ms | 0 bits | 0 Hz (DC) | Fixed-interval `traffic_model=0` |

**Key insight**: Even with high Shannon timing entropy, a gateway DUT that applies
a periodic rate-limit (e.g., token bucket refilling every 10 ms) will impose a
subtle 100 Hz rhythm on the TX rate.  This rhythm is invisible to distribution-based
metrics but clearly visible in the PSD near the first resolvable bins. The
100 us sampler also keeps a 1 ms pacing cycle below Nyquist instead of aliasing
it to DC.

### 1.2 What PSD Detects

| Pattern Detected | Frequency Range | Likely Cause |
|------------------|-----------------|--------------|
| DC spike (bin 0 dominant) | 0 Hz | Constant TX rate -- no timing variation at all |
| Sharp peak at mid frequencies | 20-200 Hz | DUT token-bucket refill, policer window, or rate-limiter cycle |
| Broad, flat spectrum | all bins | High-entropy, aperiodic TX -- ideal for stress testing |
| 50/60 Hz peaks | 50/60 Hz | AC mains coupling in physical NIC timing |
| High-frequency noise floor | > 300 Hz | TX burst jitter or packet-level artefacts |

### 1.3 Relationship to Other Entropy Metrics

```
Shannon H(delta_tsc)  --  "how varied are the gaps?"       (distribution)
Min-entropy Hinf(dtsc) --  "how dominant is the worst gap?"  (worst-case)
MI(delta_tsc; flow)   --  "do flows cluster in time?"       (temporal locality)
PSD dominant_hz       --  "is there a period in the TX?"    (frequency domain)
PSD flatness          --  "how spread is the spectrum?"      (frequency domain)
```

Shannon and MI describe sampled values and their dependence. PSD describes
periodic structure in the TX rate. Each covers a different part of the signal.

---

## 2. Architecture

### 2.1 Data Flow

```
Worker lcore (hot path)                   Master lcore (stats thread)
+-- rate_psd_account(count, tsc)          +-- rate_psd_compute()
|   per-burst accounting                  |   read completed absolute buckets
|   absolute 100 us TSC bucket            |   merge matching bucket numbers
|   lock-free ring write (512 slots)      |   Hann window
v                                         |   Cooley-Tukey 256-pt FFT
+-- ring[bucket % 512] = {count, bucket}  |   compute dominant_hz + flatness
                                          |   cache in static storage
                                          v
                                          +-- rate_psd_fill_snapshot(s)
                                              copy to stats_snapshot
                                              -> JSON / WebSocket / Prometheus
```

### 2.2 Per-Worker Ring Buffer

Each worker maintains a `struct rate_psd` with a 512-slot ring buffer:

Each slot contains a packet count and its absolute TSC bucket number. The bucket
number release-publishes a reused slot to the statistics thread.
The dashboard redraws the PSD canvas only while its section is expanded; this
does not change the 10 kHz backend sampling or the statistics payload cadence.

```c
struct rate_psd {
    struct rate_psd_slot ring[512]; // count and absolute bucket number
};
```

- **Sampling rate**: 10 kHz (100 us per slot)
- **Window**: 51.2 ms (ring size)
- **FFT window**: 25.6 ms (last 256 slots of the merged series)
- **Write path**: Lock-free, single-writer (one worker per lcore)
- **Read path**: Atomic bucket validation before and after reading a count

### 2.3 Absolute TSC Buckets

`rate_psd_account()` maps every successful TX burst to a global bucket number:

```c
uint64_t bucket_cycles = rte_get_tsc_hz() / 10000;
uint64_t bucket = tsc / bucket_cycles;
rate_psd_account_bucket(psd, count, bucket);
```

No slots need to be written during an idle period. A missing bucket number is
read as a zero count, so an idle worker cannot leave stale traffic at the end of
the analysis window.

### 2.4 Master-Side Merge

`rate_psd_compute()` selects the most recent completed global bucket and merges
matching bucket numbers from all workers:

```c
last_complete = current_bucket - 1;
rate_psd_merge_window(workers, n_workers, last_complete, merged, 512);
```

The current partial bucket is excluded. Workers that began at different times
still align because the merge uses absolute bucket numbers instead of ring-head
positions.

### 2.5 FFT Pipeline

1. **Extract the last 256 samples** from `merged[256..511]`
2. **Subtract the window mean** so that mean TX rate (DC) cannot mask rate variation
3. **Apply Hann window**: `w[n] = 0.5 x (1 - cos(2pin/(N-1)))`
   - Suppresses spectral leakage from the finite window
   - Prevents DC power from contaminating adjacent bins
4. **Cooley-Tukey radix-2 DIT FFT** (256 points, 8 stages)
   - Self-implemented -- no external FFT library dependency
   - Bit-reversal permutation + Danielson-Lanczos butterflies
5. **Power spectrum**: `P[i] = real[i]^2 + imag[i]^2`; diagnostics use only bins 1..127, excluding DC and Nyquist

### 2.6 Frequency Resolution

```
sampling rate Fs  = 10000 Hz  (100 us per sample)
FFT size N        = 256
bin width         = Fs / N = 10000 / 256 = 39.0625 Hz
Nyquist limit     = Fs / 2 = 5000 Hz
```

Each bin index `i` maps to frequency `i x 39.0625 Hz`.

Only bins 0-127 (0 Hz to Nyquist) are meaningful -- bins 128-255 are the
complex-conjugate mirror for real-valued input and are discarded.

---

## 3. Metrics

### 3.1 Dominant Frequency

```
strongest_peak_hz = max_bin_index x 39.0625
```

The fundamental detector scans significant local peaks from low to high and
selects the first peak with another significant peak near an integer multiple.
A peak must contain at least 5% of the strongest-bin power. If no supported
harmonic series is present, `fundamental_hz` falls back to
`strongest_peak_hz`. `dominant_hz` is retained as a compatibility alias for the
strongest peak.

The non-DC frequency with the highest spectral power. If the TX rate contains
a periodic component resolvable at 100 us, this metric pinpoints its period.

| dominant_hz | Interpretation | Action |
|-------------|----------------|--------|
| 0 Hz | No measurable non-DC variation: no TX samples or a constant sampled rate | Check `signal_valid`; if TX is active, no periodic variation is visible at 100 us resolution |
| 39-200 Hz | Slow periodic structure -- possible DUT TCP window, conntrack GC, or rate-limiter cycle | Frequency resolution is 39.0625 Hz; corroborate low-frequency peaks with a longer external capture |
| 200-5000 Hz | Pacing fundamental or harmonic, burst jitter, or NIC timing | Compare the peak with the pacing frequency before attributing it to the DUT |
| None (flat spectrum) | White-noise TX -- ideal randomization | Target state for maximum-entropy stress testing |

### 3.2 Spectral Flatness (Wiener Entropy)

```
flatness = exp(mean(log(max(P[i], relative_floor)))) / mean(P[i]), i=1..127
         = geometric_mean(power) / arithmetic_mean(power)
```

Range: **0 (pure tone) to 1 (white noise)**.

| Flatness | Meaning | Stress-Testing Implication |
|----------|---------|---------------------------|
| 0.0 - 0.1 | Nearly all power in one bin -- pure periodic TX | TX is highly predictable; gateway hash/ECMP sees repetitive pattern |
| 0.1 - 0.4 | Dominant peak with some spread -- structured TX | Some randomization present, but underlying periodicity persists |
| 0.4 - 0.7 | Moderate spectral spread | Acceptable for many scenarios; check dominant_hz for residual peaks |
| 0.7 - 0.9 | Broad spectrum -- good randomization | DUT sees aperiodic traffic; good conntrack/ECMP stress |
| 0.9 - 1.0 | Near-white noise -- optimal | Maximum entropy; ideal for hash-uniformity and rate-limiter bypass testing |

**Special case**: flatness = 0.0 when all bins are zero (no TX activity or
insufficient samples).  This is a "no data" sentinel, not a valid metric.

### 3.3 DC Component

Bin 0 represents the **DC component** -- the mean TX rate over the 25.6 ms window.

The DC bin is always the dominant bin when TX is constant-rate (flatness = 0).
It is **omitted from the frontend chart** because it contains no frequency
information and would visually dominate the bar chart, hiding the remaining
spectral structure.

---

## 4. Frontend Dashboard

The `/entropy` dashboard includes a **Rate PSD** section with:

### 4.1 Summary Cards

| Card | ID | Content |
|------|----|---------|
| Dominant Freq | `psd-dominant` | Peak bin frequency in Hz, updated every stats cycle |
| Spectral Flatness | `psd-flatness` | Wiener entropy (4 decimal places) |
| Resolution | static | `39.06 Hz/bin` -- constant derived from Fs/N |
| Pacing Freq | `psd-pacing-freq` | Reciprocal of the effective submit interval |

Each card has a hover tooltip explaining the metric and its interpretation.

### 4.2 Bar Chart

A 127-bin bar chart (bins 1-127) rendered on an HTML Canvas:

- **X-axis**: Frequency labels at every 25th bin (39, 1016, 1992, ... 3945 Hz)
- **Y-axis**: Magnitude label (rotated)
- **Color**: Deep blue to bright cyan gradient, matching the dashboard theme
- **Reference lines**: Solid orange at the pacing fundamental and dashed orange
  at each harmonic below 5 kHz
- **Height**: 230 px including 25 px for X-axis labels
- **Skip DC**: Bin 0 is excluded to avoid the DC component dominating the
  visual scale

### 4.3 Description Paragraph

The section includes a prose explanation below the summary cards:

> TX rate sampled at 10 kHz (100 us bins) over a 256-point Hann-windowed FFT.
> The mean rate is removed before the FFT. Chart shows bins 1..127 on a dB
> scale (DC is excluded from all diagnostics). **Dominant Freq**: peak bin
> index * 39.0625 Hz; high values indicate rate-limit cycles. **Spectral
> Flatness** (Wiener entropy): geometric mean / arithmetic mean of bin magnitudes;
> 0 = pure periodic tone, 1 = uniform white noise. Broad, flat spectra signal
> high TX entropy; sharp peaks reveal periodic structure from DUT token-bucket
> or policer cycles.

---

## 5. Diagnostic Checklist

| Symptom | PSD Metric | Likely Cause | Fix |
|---------|-----------|--------------|-----|
| PSD shows 0 bins / no data | `flatness=0`, `dominant_hz=0` | Insufficient TX activity or worker not registered | Check `auto-start: true`; verify worker lcores are active |
| No non-DC power | `signal_valid=false`, `dominant_hz=0` | No TX, or a constant packet count in every 100 us bucket | Verify TX counters; do not interpret flatness when the signal is invalid |
| Sharp peak near 117 Hz | `dominant_hz~117` (bin 3) | DUT ~10 ms rate-limit cycle | The short 25.6 ms FFT window is coarse at low frequencies; confirm with an external longer capture |
| Sharp peak near 39 Hz | `dominant_hz~39` (bin 1) | Slow token-bucket or scheduler cycle | Treat this as a 0-78 Hz band indication, not a precise 39 Hz measurement |
| Peaks at integer multiples of pacing | `flatness<0.3` | Fixed-interval burst train | Expected for `traffic_model=0`; the last peak may be clipped at the 5 kHz Nyquist limit |
| High flatness but unexpected peak | `flatness>0.7`, but `dominant_hz!=0` | Intermittent periodic artefact -- batch jitter partially masks it | Increase `batch_jitter_us` to spread the peak |
| All bins flat without peaks | `flatness~1`, `dominant_hz` small | **Ideal** -- white-noise TX | No action; maximum entropy achieved |

---

## 6. Configuration Impact

### 6.1 Parameters That Affect PSD

| Parameter | Effect on PSD |
|-----------|---------------|
| `traffic_model=0` (fixed) | Produces fixed-interval TX bursts and spectral lines at the pacing frequency and its harmonics |
| `traffic_model=1` (Poisson) | Produces aperiodic TX -> broad spectrum, flatness near 1 |
| `batch_delay_us` | Sets mean inter-burst gap; short delays push spectrum higher |
| `batch_jitter_us` | Adds randomness to gap; reduces spectral peaks, increases flatness |
| `batch` (burst size) | Larger bursts increase per-bucket count amplitude, affecting FFT magnitude |
| `entropy_adapt_gain` | Adaptive controller may inject low-frequency modulation into TX rate |
| `sample_interval` | Affects entropy computation rate; PSD operates independently at 100 us |
| `hs_rate` / `hs_mix_ratio` | Handshake CPS modulates TX rate -- may introduce PS-level peaks |

### 6.2 Recommended Baseline for Maximum PSD Flatness

```
traffic_model: 1          # Poisson arrival = natural timing entropy
batch_jitter_us: 200      # 200 uss jitter = spreads any residual peaks
sample_interval: 1        # 1 s stats window -- no impact on PSD
```

---

## 7. JSON API

### 7.1 `/api/stats` Response (psd Object)

```json
{
  "psd": {
    "dominant_hz": 195.3125,
    "strongest_peak_hz": 195.3125,
    "fundamental_hz": 117.1875,
    "spectral_flatness": 0.4231,
    "signal_valid": true,
    "mean_ppms": 64.0,
    "variation_rms_ppms": 3.25,
    "bins": [5224.72, 2751.38, 4696.54, ...]   // 256 elements
  }
}
```

| Field | Type | Description |
|-------|------|-------------|
| `dominant_hz` | float | Compatibility alias for `strongest_peak_hz` |
| `strongest_peak_hz` | float | Frequency of the highest-power non-DC bin (Hz) |
| `fundamental_hz` | float | Lowest significant harmonic-series peak, or the strongest peak when no series is found |
| `spectral_flatness` | float | Wiener entropy (0-1) |
| `signal_valid` | boolean | Whether non-DC spectral power is present; flatness is not meaningful when false |
| `mean_ppms` | float | Mean rate normalized to packets/ms; numerically equal to Kpps |
| `variation_rms_ppms` | float | RMS non-DC rate variation in packets/ms (Kpps) |
| `bins` | float[256] | Detrended power spectrum. Bins 1-127 contain analyzed positive frequencies; bin 0 and bins 128-255 are zeroed. |

### 7.2 Prometheus `/metrics`

PSD metrics are exposed as Prometheus gauges:

```
bless_psd_dominant_hz      98.44
bless_psd_strongest_peak_hz 98.44
bless_psd_fundamental_hz    98.44
bless_psd_spectral_flatness 0.423
bless_psd_signal_valid       1
```

Individual bin values are not exposed via Prometheus (256 time series per
instance would overload cardinality).  Use the JSON API for programmatic
access to the full spectrum.

---

## 8. WebSocket Broadcast

The `ws_broadcast_stats()` loop (3 Hz) includes PSD data in every stats
snapshot sent to connected WebSocket clients.  The entropy dashboard's
`render()` function updates the PSD chart on each message.

A **startup guard** (`if (s->json_len == 0) return;`) prevents the
broadcast thread from sending empty frames before the first snapshot
is computed -- this avoids `JSON.parse("")` errors on the client.

---

## 9. Implementation Reference

### 9.1 Key Files

| File | Role |
|------|------|
| `include/rate_psd.h` | Data structures (`struct rate_psd`), API, global registry |
| `src/rate_psd.c` | Ring buffer accounting, FFT, master compute, snapshot fill |
| `include/server.h` | `struct stats_snapshot` PSD fields (`psd_dominant_hz`, etc.) |
| `src/metric.c` | JSON serialization of PSD bins via cJSON |
| `src/worker.c` | Registration + hot-path `rate_psd_account()` call after TX burst |
| `src/entropy_html.inc` | Frontend dashboard HTML/CSS/JS for PSD section |
| `src/server.c` | `ws_broadcast_stats()` with zero-length guard |

### 9.2 FFT Implementation

```c
// Standalone 256-point radix-2 DIT FFT
static void fft_256(double *real, double *imag)
{
    // 1. Bit-reversal permutation
    for (int i = 1, j = 0; i < 256; i++) {
        int bit = 256 >> 1;
        for (; (j & bit) != 0; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { swap(real[i], real[j]); swap(imag[i], imag[j]); }
    }

    // 2. Danielson-Lanczos butterflies (8 stages, len=2,4,8,...,256)
    for (int len = 2; len <= 256; len <<= 1) {
        double ang = 2.0 * M_PI / len;
        double wlen_r = cos(ang), wlen_i = -sin(ang);
        for (int i = 0; i < 256; i += len) {
            double wr = 1.0, wi = 0.0;
            for (int j = 0; j < len/2; j++) {
                int u = i + j, v = u + len/2;
                double tr = wr * real[v] - wi * imag[v];
                double ti = wr * imag[v] + wi * real[v];
                real[v] = real[u] - tr; imag[v] = imag[u] - ti;
                real[u] += tr;           imag[u] += ti;
                // twiddle advance: w *= wlen
                double wr_next = wr * wlen_r - wi * wlen_i;
                wi = wr * wlen_i + wi * wlen_r;
                wr = wr_next;
            }
        }
    }
}
```

Complexity: O(N log N) = 256 x 8 = 2048 butterfly operations per stats cycle.

### 9.3 Snapshot Integration

```c
// In stats computation (worker.c):
rate_psd_compute();                    // run FFT, cache results
rate_psd_fill_snapshot(active_snap);   // copy into snapshot

// In JSON serialization (metric.c):
cJSON *psd = cJSON_CreateObject();
cJSON_AddNumberToObject(psd, "dominant_hz",      ent->psd_dominant_hz);
cJSON_AddNumberToObject(psd, "spectral_flatness", ent->psd_spectral_flatness);
for (int i = 0; i < 256; i++)
    cJSON_AddItemToArray(bins, cJSON_CreateNumber(ent->psd_bins[i]));
cJSON_AddItemToObject(root, "psd", psd);
```

### 9.4 Performance Budget

| Component | Cost | Notes |
|-----------|------|-------|
| `rate_psd_account()` (hot path) | One bucket calculation and atomic slot update per burst | No work proportional to an idle gap |
| `rate_psd_compute()` (master) | 512 bucket reads per worker plus one FFT | Runs once per stats cycle |
| JSON serialization (256 bins) | ~2 KB | ~2900 bytes in final JSON |
| Canvas rendering (127 bars) | ~1 ms in browser | Device-pixel-ratio-aware canvas |

---

## 10. Maintenance and Extension

### 10.1 Changing FFT Size

1. Update `PSD_FFT_SIZE` in `include/rate_psd.h`
2. Update `psd_bins` array size in `include/server.h`
3. Adjust `fft_256()` loop bounds or replace with a general-purpose FFT
4. Update the frontend bar chart to use the new bin count
5. Update the bin-width constant and description text
6. Update this document

### 10.2 Adding New PSD-Derived Metrics

1. Compute the metric in `rate_psd_compute()` after the FFT loop
2. Store it in the static cache alongside `cached_dominant_hz`
3. Add a field to `struct stats_snapshot` in `include/server.h`
4. Serialize in `rate_psd_fill_snapshot()` and `metric.c`
5. Add a summary card in `src/entropy_html.inc`
6. Document in this file

### 10.3 Diagnostic Notes

- **Net null PMD caveat**: With `net_null` (CI smoke test), TX rate is
  perfectly constant (packets are dropped, not queued).  Expect PSD to show
  pure DC (flatness = 0, dominant_hz = 0).  Real NICs with hardware pacing
  produce more interesting spectra.
- **VXLAN impact**: VXLAN encapsulation doubles packet count per logical TX
  (inner + outer), increasing FFT magnitude but not affecting frequency
  structure -- PSD sees the aggregate rate.
- **Multi-queue**: Each worker has an independent ring. Absolute bucket numbers
  keep the merged time series aligned across queues.
