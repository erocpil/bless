#!/usr/bin/env python3
"""Multi-queue TX performance sweep -- q=1,2,4,8,16 queues, 50M packets per run."""

import subprocess, time, sys, os, json, yaml, urllib.request, signal, tempfile
from pathlib import Path

PROJECT = Path(__file__).resolve().parent.parent
TEMPLATE = PROJECT / "conf" / "config.yaml.test"
BLESS = PROJECT / "build" / "release-static" / "bin" / "bless"
METRICS_URL = "http://127.0.0.1:8000/metrics"

QUEUE_COUNTS = [1, 2, 4, 8, 16]
NUM_PKTS = 50_000_000
BATCH = 512
WARMUP_S = 3
SAMPLE_INTERVAL = 1.0
TIMEOUT_S = 600

def load_template():
    with open(TEMPLATE) as f:
        return yaml.safe_load(f)

def gen_config(q, lcores):
    """Generate YAML config for q queues with given lcore list."""
    cfg = load_template()
    cfg["dpdk"]["l"] = lcores
    cfg["dpdk"]["n"] = max(2, q // 2)  # memory channels
    cfg["dpdk"]["log-level"] = "emerg"  # quiet
    cfg["injector"]["q"] = q
    cfg["injector"]["T"] = 1
    cfg["injector"]["batch"] = BATCH
    cfg["injector"]["num"] = NUM_PKTS
    cfg["system"]["daemonize"] = False
    return cfg

def kill_bless():
    subprocess.run(["pkill", "-f", "build/release.*bless"], stderr=subprocess.DEVNULL)
    time.sleep(1)

def fetch_metrics():
    try:
        with urllib.request.urlopen(METRICS_URL, timeout=2) as resp:
            return resp.read().decode()
    except Exception:
        return ""

def parse_tx_mpps(text):
    """Extract aggregate TX MPPS from Prometheus metrics."""
    total = 0.0
    for line in text.split("\n"):
        if line.startswith("bless_tx_mpps{"):
            try:
                total += float(line.rsplit(" ", 1)[1])
            except (ValueError, IndexError):
                pass
    return total

def run_test(q, lcores):
    print(f"\n{'='*60}")
    print(f"q={q:2d}  lcores={lcores}  packets={NUM_PKTS:,}  batch={BATCH}")
    print(f"{'='*60}")

    cfg = gen_config(q, lcores)
    with tempfile.NamedTemporaryFile(mode='w', suffix='.yaml', delete=False) as f:
        yaml.dump(cfg, f, default_flow_style=False)
        cfg_path = f.name

    kill_bless()
    time.sleep(1)

    proc = subprocess.Popen(
        [str(BLESS), cfg_path],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )

    print(f"  pid={proc.pid}  warmup {WARMUP_S}s...")
    time.sleep(WARMUP_S)

    samples = []
    deadline = time.time() + TIMEOUT_S

    while time.time() < deadline:
        if proc.poll() is not None:
            print(f"  bless exited (rc={proc.returncode})")
            break

        text = fetch_metrics()
        pps = parse_tx_mpps(text) if text else 0
        samples.append(pps)
        if pps > 0:
            print(f"  {pps:8.2f} MPPS", end="\r")
        time.sleep(SAMPLE_INTERVAL)

    # ensure killed
    try:
        proc.terminate()
        proc.wait(timeout=5)
    except Exception:
        proc.kill()

    os.unlink(cfg_path)
    kill_bless()

    if not samples:
        print(f"  NO DATA")
        return 0
    if all(s < 0.1 for s in samples):
        print(f"  all ~0 MPPS (bless may have exited before warmup)")
        return 0

    # Use median of non-zero P50 samples
    nonzero = [s for s in samples if s > 0]
    if not nonzero:
        return 0
    nonzero.sort()
    p50 = nonzero[len(nonzero) // 2]

    print(f"\n  {'P50:':>8} {p50:8.2f} MPPS  (samples={len(samples)}, warmup={WARMUP_S}s)")
    return p50

def main():
    print("=== Multi-Queue TX Performance Sweep ===")
    print(f"  NIC: ConnectX-6 PF   packets/run: {NUM_PKTS:,}   batch: {BATCH}")
    print()

    results = []
    for q in QUEUE_COUNTS:
        # lcore list: 0 (control) + 1..q (workers)
        lcores = f"0,1-{q}" if q > 1 else "0,1"
        p50 = run_test(q, lcores)
        if p50 > 0:
            results.append((q, p50))

    print(f"\n{'='*60}")
    print(f"{'q':>4s}  {'MPPS (P50)':>12s}  {'pkt/s/core':>14s}")
    print(f"{'-'*60}")
    for q, mpps in results:
        pps_per_core = (mpps * 1e6) / q
        print(f"{q:4d}  {mpps:12.2f}  {pps_per_core:14,.0f}")
    print()

    # Save to file for reference
    out_path = PROJECT / "logs" / "bench_mq_tx.txt"
    out_path.parent.mkdir(exist_ok=True)
    with open(out_path, "w") as f:
        f.write(f"Multi-Queue TX Sweep  {time.strftime('%Y-%m-%d %H:%M')}\n")
        f.write(f"NIC: ConnectX-6 PF  packets={NUM_PKTS:,}  batch={BATCH}\n\n")
        f.write(f"{'q':>4s}  {'MPPS (P50)':>12s}  {'pkt/s/core':>14s}\n")
        for q, mpps in results:
            pps_per_core = (mpps * 1e6) / q
            f.write(f"{q:4d}  {mpps:12.2f}  {pps_per_core:14,.0f}\n")
    print(f"  results → {out_path}")

if __name__ == "__main__":
    main()
