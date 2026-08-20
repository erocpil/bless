#!/usr/bin/env python3
"""2a: single-core PPS ceiling — sweep batch size."""

import subprocess, time, urllib.request, sys

BATCHES = [1, 2, 4, 8, 16, 32, 64, 128, 256, 512]
SAMPLES = 5
SAMPLE_INTERVAL = 2     # seconds between samples
WARMUP = 5              # seconds — must be > timer_period * 3 (first
                        # cycle skips rates, second populates them)
MAX_RETRIES = 15        # wait up to WARMUP + MAX_RETRIES*INTERVAL for data

def collect_pps(max_attempts: int, interval: int, want: int) -> list[float]:
    pps_vals = []
    for attempt in range(max_attempts):
        time.sleep(interval)
        try:
            with urllib.request.urlopen("http://127.0.0.1:8000/metrics", timeout=5) as resp:
                text = resp.read().decode()
        except Exception:
            continue
        for line in text.splitlines():
            if line.startswith("bless_tx_mpps "):
                try:
                    mpps = float(line.split()[1])
                    if mpps > 0:
                        pps_vals.append(mpps * 1_000_000)
                except (ValueError, IndexError):
                    pass
                break
        if len(pps_vals) >= want:
            break
    return pps_vals

results = []
for batch in BATCHES:
    sys.stdout.write(f"batch={batch:>3}  ")
    sys.stdout.flush()

    proc = subprocess.Popen(
        ["./build/release-static/bin/bless", "conf/config-bench.yaml",
         "--", f"--batch={batch}", "-T", "1"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    time.sleep(WARMUP)

    pps = collect_pps(MAX_RETRIES, SAMPLE_INTERVAL, SAMPLES)
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()

    if pps:
        pps.sort()
        median = pps[len(pps) // 2]
        mean = sum(pps) / len(pps)
        print(f"median={median/1e6:.3f} MPPS  mean={mean/1e6:.3f}  "
              f"n={len(pps)}")
        results.append((batch, median, mean, len(pps)))
    else:
        print("NO DATA")
        results.append((batch, 0, 0, 0))

print("\n=== 2a Results: Single-Core PPS Ceiling (net_null PMD) ===")
print(f"{'Batch':>6}  {'Median MPPS':>12}  {'Mean MPPS':>10}  {'Samples':>8}")
print("-" * 52)
for batch, median, mean, n in results:
    if median > 0:
        print(f"{batch:>6}  {median/1e6:>12.3f}  {mean/1e6:>10.3f}  {n:>8}")

# Find sweet spot
best = max(results, key=lambda x: x[1])
print(f"\nSweet spot: batch={best[0]} → {best[1]/1e6:.3f} MPPS")
