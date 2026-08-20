#!/usr/bin/env python3
"""H3: packet-size profile on ConnectX-6 PF — sweep IMIX sizes."""

import subprocess, time, urllib.request, sys, os, tempfile, yaml

SIZES = [64, 128, 256, 512, 1024, 1500, None]  # None = IMIX mix
IMIX_MIX = [64, 594, 1500]
BATCH = 128
SAMPLES = 5
SAMPLE_INTERVAL = 2
WARMUP = 5
MAX_RETRIES = 12
BENCH_YAML = "conf/config-hw-pf.yaml"
NUM_PKTS = 30000000   # 30M — enough for stable PPS, short enough for sweep


def load_template():
    with open(BENCH_YAML) as f:
        return yaml.safe_load(f)


def collect_metrics(max_attempts, interval, want):
    results = []
    for _ in range(max_attempts):
        time.sleep(interval)
        try:
            with urllib.request.urlopen(
                "http://127.0.0.1:8000/metrics", timeout=5
            ) as resp:
                text = resp.read().decode()
        except Exception:
            continue
        mpps = gbps = 0.0
        for line in text.splitlines():
            if line.startswith("bless_tx_mpps "):
                try: mpps = float(line.split()[1])
                except: pass
            if line.startswith("bless_tx_gbps "):
                try: gbps = float(line.split()[1])
                except: pass
        if mpps > 0:
            results.append((mpps, gbps))
        if len(results) >= want:
            break
    return results


def run_one(label, imix):
    cfg = load_template()
    if imix is None:
        cfg["bless"]["ether"]["imix"] = IMIX_MIX
        label = "IMIX mix"
    else:
        cfg["bless"]["ether"]["imix"] = [imix]
        label = f"{imix}B"

    sys.stdout.write(f"  {label:<16} ")
    sys.stdout.flush()

    with tempfile.NamedTemporaryFile(mode="w", suffix=".yaml", delete=False) as f:
        yaml.dump(cfg, f)
        tmp_path = f.name

    proc = subprocess.Popen(
        ["./build/release-static/bin/bless", tmp_path,
         "--", f"--batch={BATCH}", "-T", "1", f"--num={NUM_PKTS}"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    time.sleep(WARMUP)

    data = collect_metrics(MAX_RETRIES, SAMPLE_INTERVAL, SAMPLES)
    proc.terminate()
    try: proc.wait(timeout=5)
    except: proc.kill(); proc.wait()

    os.unlink(tmp_path)

    if not data:
        print("NO DATA")
        return (0, 0, 0)

    data.sort(key=lambda x: x[0])
    median = data[len(data) // 2]
    print(f"PPS={median[0]:.3f} MPPS  Gbps={median[1]:.3f}  n={len(data)}")
    return (median[0], median[1], len(data))


def main():
    print("=== H3: Packet-Size Profile (PF, batch=128) ===\n")

    results = []
    for size in SIZES:
        mpps, gbps, n = run_one(None, size)
        if mpps > 0:
            avg = size if size else sum(IMIX_MIX) / len(IMIX_MIX)
            results.append((size, avg, mpps, gbps, n))

    print(f"\n{'Frame':>6}  {'~Bytes':>6}  {'MPPS':>8}  {'Gbps':>8}  {'PPS':>12}  {'Samples':>8}")
    print("-" * 58)
    for size, avg, mpps, gbps, n in results:
        label = f"{size}B" if size else "IMIX"
        pps = int(mpps * 1_000_000)
        print(f"{label:>6}  {avg:>6.0f}  {mpps:>8.3f}  {gbps:>8.3f}  {pps:>12,}  {n:>8}")

    if results:
        best = max(results, key=lambda x: x[3])
        print(f"\nGbps ceiling: {best[3]:.3f} Gbps at {best[1]:.0f}-byte frames")


if __name__ == "__main__":
    main()
