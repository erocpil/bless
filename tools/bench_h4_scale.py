#!/usr/bin/env python3
"""H4 — Multi-core scaling sweep on ConnectX-6 PF (mlx5 PMD).

Sweeps 1, 2, 4, 8, 16, 32 worker cores at batch=128, IMIX=[64], UDP-only.
Writes per-run temp YAML, launches bless, polls /metrics, reports aggregate MPPS.
"""

import subprocess
import time
import sys
import os
import json
import yaml
import urllib.request
import signal
import tempfile
from pathlib import Path

PROJECT = Path(__file__).resolve().parent.parent
TEMPLATE = PROJECT / "conf" / "config-hw-pf.yaml"
BLESS = PROJECT / "build" / "release-static" / "bin" / "bless"
METRICS_URL = "http://127.0.0.1:8000/metrics"
CORES = [1, 2, 4, 8, 16, 32]
NUM_PKTS = 50_000_000
BATCH = 128
SAMPLES_PER_RUN = 5       # 5 PPS datapoints
SAMPLE_COOLDOWN_S = 2     # seconds between samples
WARMUP_S = 3              # wait for PPS to stabilise
TIMEOUT_S = 300           # max wait for bless to finish

def load_yaml(path):
    with open(path) as f:
        return yaml.safe_load(f)

def save_yaml(cfg, path):
    with open(path, "w") as f:
        yaml.dump(cfg, f, default_flow_style=False)

def parse_metrics(text):
    """Parse Prometheus text into {name: value} dict."""
    out = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 1)
        if len(parts) == 2:
            try:
                out[parts[0]] = float(parts[1])
            except ValueError:
                pass
    return out

def fetch_metric(name):
    try:
        resp = urllib.request.urlopen(METRICS_URL, timeout=5)
        data = parse_metrics(resp.read().decode())
        return data.get(name, None)
    except Exception:
        return None

def wait_for_port(port=8000, timeout=30):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            urllib.request.urlopen(f"http://127.0.0.1:{port}/metrics", timeout=3)
            return True
        except Exception:
            time.sleep(0.5)
    return False

def kill_port(port=8000):
    try:
        subprocess.run(
            f"ss -tlnp | grep :{port} | awk '{{print $NF}}' | grep -oP 'pid=\\K\\d+' | xargs -r kill -9",
            shell=True, timeout=5, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception:
        pass

def run_bless(config_path, num_workers, num_pkts, batch):
    """Launch bless, collect PPS samples, return aggregate MPPS."""
    cmd = [
        str(BLESS), str(config_path),
        "--", f"--num={num_pkts}", f"--batch={batch}",
        "-T", "1"  # 1-second stats interval
    ]
    print(f"  launch: {' '.join(cmd)}")

    proc = subprocess.Popen(
        cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        cwd=str(PROJECT))

    # Wait for metrics server
    if not wait_for_port(8000, timeout=60):
        print("  ERROR: metrics server did not come up")
        proc.kill()
        proc.wait()
        return None

    # Warmup
    time.sleep(WARMUP_S)

    # Collect samples
    samples = []
    for i in range(SAMPLES_PER_RUN):
        time.sleep(SAMPLE_COOLDOWN_S)
        mpps = fetch_metric("bless_tx_mpps")
        if mpps is not None and mpps > 0:
            samples.append(mpps)
            print(f"    sample {i+1}/{SAMPLES_PER_RUN}: {mpps:.3f} MPPS")

    # Wait for bless to finish
    try:
        proc.wait(timeout=TIMEOUT_S)
    except subprocess.TimeoutExpired:
        print("  WARNING: bless timeout, killing")
        proc.kill()
        proc.wait()

    # Also check dpdk_opackets
    time.sleep(2)
    opackets = fetch_metric("dpdk_opackets")
    if opackets:
        print(f"  dpdk_opackets: {opackets:,.0f}")

    # Cleanup
    kill_port(8000)
    time.sleep(1)

    if not samples:
        return None

    samples.sort()
    p50 = samples[len(samples) // 2]
    return {
        "cores": num_workers,
        "aggregate_mpps": p50,  # P50 of 5 samples = best estimate of steady-state
        "p50_mpps": p50,
        "per_core_mpps": p50 / num_workers,
        "samples": samples,
        "dpdk_opackets": opackets,
        "exit_code": proc.returncode,
    }

def main():
    print("=" * 60)
    print("H4 — Multi-Core Scaling Sweep (ConnectX-6 PF, mlx5 PMD)")
    print("=" * 60)
    print(f"Batch: {BATCH}, IMIX=[64], UDP-only, {NUM_PKTS/1e6:.0f}M packets/run")
    print()

    if not BLESS.exists():
        print(f"ERROR: bless binary not found at {BLESS}")
        sys.exit(1)

    template = load_yaml(TEMPLATE)
    results = []

    for n in CORES:
        print(f"\n--- {n} core{'s' if n > 1 else ''} ---")
        kill_port(8000)
        time.sleep(1)

        cfg = load_yaml(TEMPLATE)  # fresh copy
        cfg["dpdk"]["l"] = f"0-{n}"
        cfg["dpdk"]["n"] = n + 1    # 1 master + n workers
        cfg["injector"]["q"] = n     # 1 TX queue per worker
        cfg["injector"]["num"] = NUM_PKTS * n  # scale total pkts with cores

        with tempfile.NamedTemporaryFile(
            mode="w", suffix=".yaml", prefix="h4_", delete=False,
            dir="/tmp") as tf:
            save_yaml(cfg, tf.name)
            tmp_path = tf.name

        result = run_bless(tmp_path, n, cfg["injector"]["num"], BATCH)
        os.unlink(tmp_path)

        if result:
            results.append(result)

        time.sleep(2)  # let NIC quiesce between runs

    # Print summary
    print("\n" + "=" * 60)
    print("H4 RESULTS — Multi-Core Scaling")
    print("=" * 60)
    print(f"{'Cores':>6} {'Aggregate MPPS':>15} {'P50 MPPS':>10} {'Per-Core MPPS':>15} {'Efficiency':>10}")
    print("-" * 60)

    baseline = None
    for r in results:
        if r["cores"] == 1:
            baseline = r["aggregate_mpps"]
        eff = (r["aggregate_mpps"] / (baseline * r["cores"]) * 100) if baseline else 0
        print(f"{r['cores']:>6} {r['aggregate_mpps']:>15.3f} {r['p50_mpps']:>10.3f} {r['per_core_mpps']:>15.3f} {eff:>9.1f}%")

    print("-" * 60)
    if baseline:
        for r in results:
            if r["cores"] > 1:
                eff = r["aggregate_mpps"] / (baseline * r["cores"]) * 100
                print(f"  {r['cores']:>2} cores: {eff:.1f}% linear efficiency")

if __name__ == "__main__":
    main()
