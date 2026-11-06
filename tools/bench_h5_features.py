#!/usr/bin/env python3
"""H5 — Feature overhead matrix on ConnectX-6 PF (mlx5 PMD).

Measures the per-feature MPPS cost for each composable capability:
  Baseline  → plain UDP tx (reference)
  +VXLAN    → VXLAN encapsulation (100% of packets)
  +Erroneous→ erroneous injection (~10% of packets)
  +Entropy  → multi-protocol (ARP+ICMP+TCP+UDP) + VXLAN + erroneous
"""

import subprocess
import time
import sys
import os
import yaml
import urllib.request
import tempfile
from pathlib import Path

PROJECT = Path(__file__).resolve().parent.parent
TEMPLATE = PROJECT / "conf" / "config-hw-pf.yaml"
BLESS = PROJECT / "build" / "release-static" / "bin" / "bless"
METRICS_URL = "http://127.0.0.1:8000/metrics"
NUM_PKTS = 50_000_000
BATCH = 128
SAMPLES_PER_RUN = 5
SAMPLE_COOLDOWN_S = 2
WARMUP_S = 3
TIMEOUT_S = 300


def load_yaml(path):
    with open(path) as f:
        return yaml.safe_load(f)


def save_yaml(cfg, path):
    with open(path, "w") as f:
        yaml.dump(cfg, f, default_flow_style=False)


def parse_metrics(text):
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


def wait_for_port(port=8000, timeout=60):
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
            f"ss -tlnp | grep :{port} | awk '{{print $NF}}' | grep -oP 'pid=\\\\K\\\\d+' | xargs -r kill -9",
            shell=True, timeout=5, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except Exception:
        pass


def run_bless(config_path, label):
    """Launch bless, collect PPS samples, return aggregate MPPS."""
    cmd = [
        str(BLESS), str(config_path),
        "--", f"--num={NUM_PKTS}", f"--batch={BATCH}",
        "-T", "1"
    ]
    print(f"  launch: bless {config_path}")

    proc = subprocess.Popen(
        cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        cwd=str(PROJECT))

    if not wait_for_port(8000, timeout=60):
        print("  ERROR: metrics server did not come up")
        proc.kill()
        proc.wait()
        return None

    time.sleep(WARMUP_S)

    samples = []
    for i in range(SAMPLES_PER_RUN):
        time.sleep(SAMPLE_COOLDOWN_S)
        mpps = fetch_metric("bless_tx_mpps")
        if mpps is not None and mpps > 0:
            samples.append(mpps)
            print(f"    sample {i+1}/{SAMPLES_PER_RUN}: {mpps:.3f} MPPS")

    try:
        proc.wait(timeout=TIMEOUT_S)
    except subprocess.TimeoutExpired:
        print("  WARNING: bless timeout, killing")
        proc.kill()
        proc.wait()

    print(f"  exit_code: {proc.returncode}")

    time.sleep(2)
    opackets = fetch_metric("dpdk_opackets")
    if opackets:
        print(f"  dpdk_opackets: {opackets:,.0f}")

    kill_port(8000)
    time.sleep(1)

    if not samples:
        return None

    samples.sort()
    p50 = samples[len(samples) // 2]
    return {
        "label": label,
        "aggregate_mpps": p50,
        "p50_mpps": p50,
        "samples": samples,
        "dpdk_opackets": opackets,
        "exit_code": proc.returncode,
    }


SCENARIOS = [
    ("Baseline", lambda c: None),  # template as-is

    ("+VXLAN", lambda c: c["bless"]["vxlan"].update({
        "enable": True,
        "ratio": 100,
        "ether": {
            "mtu": 0,
            "copy-payload": False,
            "dst": "02:00:00:00:00:02",
            "type": {
                "ipv4": {
                    "src": ["172.16.0.1:100"],
                    "dst": ["192.168.2.1"],
                    "udp": {
                        "src": [4789],
                        "dst": [4789],
                        "payload": "v",
                    },
                },
            },
        },
    })),

    ("+Erroneous", lambda c: c["bless"]["erroneous"].update({
        "ratio": 100,   # ~9.8% of packets mutated
        "class": {
            "ipv4": ["version", "ihl", "cksum"],
            "tcp": ["syn_flood"],
        },
    })),

    ("+Entropy", lambda c: (
        c["bless"]["vxlan"].update({
            "enable": True,
            "ratio": 30,
            "ether": {
                "mtu": 0,
                "copy-payload": False,
                "dst": "02:00:00:00:00:02",
                "type": {
                    "ipv4": {
                        "src": ["172.16.0.1:100"],
                        "dst": ["192.168.2.1"],
                        "udp": {
                            "src": [4789],
                            "dst": [4789],
                            "payload": "v",
                        },
                    },
                },
            },
        }),
        c["bless"]["erroneous"].update({
            "ratio": 50,
            "class": {
                "ipv4": ["version", "ihl", "dscp", "cksum"],
                "tcp": ["syn_flood", "window", "cksum"],
                "udp": ["len", "cksum"],
            },
        }),
        c["injector"].update({
            "arp": 10,
            "icmp": 20,
            "tcp": 30,
            "udp": 40,
        }),
        c["bless"]["ether"]["type"].update({
            "arp": {
                "src": ["10.0.0.1"],
                "dst": ["10.0.0.2"],
            },
            "icmp": {
                "ident": [0x10, 0x20, 0x30],
            },
            "tcp": {
                "src": "30000+99",
                "dst": "80+9",
                "payload": "e",
            },
        }),
    )),
]


def main():
    print("=" * 60)
    print("H5 — Feature Overhead Matrix (ConnectX-6 PF, mlx5 PMD)")
    print("=" * 60)
    print(f"1 core, batch={BATCH}, {NUM_PKTS/1e6:.0f}M pkts/run, "
          f"{SAMPLES_PER_RUN} samples P50")
    print()

    if not BLESS.exists():
        print(f"ERROR: bless binary not found at {BLESS}")
        sys.exit(1)

    results = []

    for label, builder in SCENARIOS:
        print(f"\n--- {label} ---")
        kill_port(8000)
        time.sleep(2)

        cfg = load_yaml(TEMPLATE)
        builder(cfg)

        with tempfile.NamedTemporaryFile(
            mode="w", suffix=".yaml", prefix="h5_", delete=False,
            dir="/tmp") as tf:
            save_yaml(cfg, tf.name)
            tmp_path = tf.name

        result = run_bless(tmp_path, label)
        os.unlink(tmp_path)

        if result:
            results.append(result)

        time.sleep(2)

    # Summary
    print("\n" + "=" * 60)
    print("H5 RESULTS — Feature Overhead")
    print("=" * 60)

    baseline_mpps = None
    for r in results:
        if r["label"] == "Baseline":
            baseline_mpps = r["aggregate_mpps"]
            break

    if baseline_mpps:
        print(f"Baseline: {baseline_mpps:.3f} MPPS")
        print(f"{'Scenario':>14} {'MPPS':>8} {'Delta':>8} {'Overhead':>8}")
        print("-" * 42)
        for r in results:
            delta = r["aggregate_mpps"] - baseline_mpps
            overhead = (delta / baseline_mpps * 100) if baseline_mpps else 0
            print(f"{r['label']:>14} {r['aggregate_mpps']:>8.3f} "
                  f"{delta:>+8.3f} {overhead:>+7.1f}%")

        # Per-feature breakdown
        print("\n--- Per-Feature Cost ---")
        for r in results:
            if r["label"] == "Baseline":
                continue
            delta = r["aggregate_mpps"] - baseline_mpps
            overhead = abs(delta / baseline_mpps * 100) if baseline_mpps else 0
            pps_cost = abs(delta) * 1_000_000
            print(f"  {r['label']:>14}: {overhead:.1f}% overhead "
                  f"({pps_cost:,.0f} PPS)")
    else:
        print("ERROR: no baseline data")


if __name__ == "__main__":
    main()
