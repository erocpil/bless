#!/usr/bin/env python3
"""2c: multi-core scaling — sweep lcore count, measure aggregate PPS."""

import subprocess, time, urllib.request, sys, os, tempfile, yaml

NCORES = [1, 2, 4, 8, 16, 32]
BATCH = 128
IMIX = [64]                    # worst-case PPS, best scaling stress
SAMPLES = 5
SAMPLE_INTERVAL = 2
WARMUP_BASE = 5                # baseline warmup
MAX_RETRIES = 20
BENCH_YAML = "conf/config-bench.yaml"


def load_template():
    with open(BENCH_YAML) as f:
        return yaml.safe_load(f)


def collect_mpps(max_attempts: int, interval: int, want: int) -> list[float]:
    results = []
    for attempt in range(max_attempts):
        time.sleep(interval)
        try:
            with urllib.request.urlopen("http://127.0.0.1:8000/metrics", timeout=5) as resp:
                text = resp.read().decode()
        except Exception:
            continue
        mpps = 0.0
        for line in text.splitlines():
            if line.startswith("bless_tx_mpps "):
                try:
                    mpps = float(line.split()[1])
                except (ValueError, IndexError):
                    pass
                break
        if mpps > 0:
            results.append(mpps)
        if len(results) >= want:
            break
    return results


def run_one(nworkers: int) -> tuple[float, float, int]:
    cfg = load_template()
    max_lcore = nworkers  # core 0 = master, 1..nworkers = workers
    cfg["dpdk"]["l"] = f"0-{max_lcore}"
    cfg["dpdk"]["n"] = max_lcore + 1
    cfg["bless"]["ether"]["imix"] = IMIX

    sys.stdout.write(f"  {nworkers:>2} cores   ")
    sys.stdout.flush()

    with tempfile.NamedTemporaryFile(mode="w", suffix=".yaml", delete=False) as f:
        yaml.dump(cfg, f)
        tmp_path = f.name

    warmup = WARMUP_BASE + nworkers  # DPDK init takes longer with more lcores

    proc = subprocess.Popen(
        ["./build/release-static/bin/bless", tmp_path,
         "--", f"--batch={BATCH}", "-T", "1"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    time.sleep(warmup)

    data = collect_mpps(MAX_RETRIES, SAMPLE_INTERVAL, SAMPLES)
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()

    os.unlink(tmp_path)

    if not data:
        print("NO DATA")
        return (0, 0, 0)

    data.sort()
    median = data[len(data) // 2]
    print(f"{median:.3f} MPPS  n={len(data)}")
    return (median, len(data))


def main():
    print("=== 2c: Multi-Core Scaling (batch=128, IMIX=[64], net_null) ===\n")
    print(f"{'Cores':>6}  {'Aggregate MPPS':>15}  {'Per-core MPPS':>14}  {'Efficiency':>10}  {'Samples':>8}")
    print("-" * 58)

    results = []
    baseline_mpps = 0
    for ncores in NCORES:
        mpps, nsamples = run_one(ncores)
        if mpps > 0 and nsamples > 0:
            results.append((ncores, mpps, nsamples))

    if not results:
        print("NO RESULTS")
        return

    baseline_mpps = results[0][1]  # 1-core MPPS

    print(f"\n{'Cores':>6}  {'Aggregate MPPS':>15}  {'Per-core MPPS':>14}  {'Efficiency':>10}  {'Samples':>8}")
    print("-" * 58)
    for ncores, mpps, nsamples in results:
        per_core = mpps / ncores
        if ncores == 1:
            eff = 100.0
        else:
            eff = (mpps / (ncores * baseline_mpps)) * 100
        print(f"{ncores:>6}  {mpps:>15.3f}  {per_core:>14.3f}  {eff:>9.1f}%  {nsamples:>8}")

    # Summary
    print(f"\nBaseline (1-core): {baseline_mpps:.3f} MPPS")
    if len(results) >= 2:
        last = results[-1]
        last_eff = (last[1] / (last[0] * baseline_mpps)) * 100
        print(f"  {last[0]}-core: {last[1]:.3f} MPPS, efficiency={last_eff:.1f}%")


if __name__ == "__main__":
    main()
