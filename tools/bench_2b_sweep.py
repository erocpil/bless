#!/usr/bin/env python3
"""2b: packet-size profile — sweep IMIX sizes, measure PPS vs Gbps."""

import subprocess, time, urllib.request, sys, os, tempfile, yaml

SIZES = [64, 128, 256, 512, 1024, 1500, None]  # None = standard IMIX mix
IMIX_MIX = [64, 594, 1500]
BATCH = 128
SAMPLES = 5
SAMPLE_INTERVAL = 2
WARMUP = 5
MAX_RETRIES = 15
BENCH_YAML = "conf/config-bench.yaml"


def load_template():
    with open(BENCH_YAML) as f:
        return yaml.safe_load(f)


def collect_pps(max_attempts: int, interval: int, want: int) -> list[tuple[float, float]]:
    """Return list of (mpps, gbps) tuples."""
    results = []
    for attempt in range(max_attempts):
        time.sleep(interval)
        try:
            with urllib.request.urlopen("http://127.0.0.1:8000/metrics", timeout=5) as resp:
                text = resp.read().decode()
        except Exception:
            continue
        mpps = gbps = 0.0
        for line in text.splitlines():
            if line.startswith("bless_tx_mpps "):
                try:
                    mpps = float(line.split()[1])
                except (ValueError, IndexError):
                    pass
            if line.startswith("bless_tx_gbps "):
                try:
                    gbps = float(line.split()[1])
                except (ValueError, IndexError):
                    pass
        if mpps > 0:
            results.append((mpps, gbps))
        if len(results) >= want:
            break
    return results


def run_one(label: str, imix: list[int] | None) -> tuple[float, float, int]:
    cfg = load_template()
    if imix is None:
        cfg["bless"]["ether"]["imix"] = IMIX_MIX
        label = f"IMIX " + "/".join(str(s) for s in IMIX_MIX)
    else:
        cfg["bless"]["ether"]["imix"] = [imix] if isinstance(imix, int) else imix
        label = f"{imix}B" if isinstance(imix, int) else label
    # Ensure MTU is 0 so IMIX controls exact size
    cfg["bless"]["ether"]["mtu"] = 0

    sys.stdout.write(f"  {label:<16} ")
    sys.stdout.flush()

    with tempfile.NamedTemporaryFile(mode="w", suffix=".yaml", delete=False) as f:
        yaml.dump(cfg, f)
        tmp_path = f.name

    proc = subprocess.Popen(
        ["./build/release-static/bin/bless", tmp_path,
         "--", f"--batch={BATCH}", "-T", "1"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    time.sleep(WARMUP)

    data = collect_pps(MAX_RETRIES, SAMPLE_INTERVAL, SAMPLES)
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

    # Sort by MPPS, take median
    data.sort(key=lambda x: x[0])
    median = data[len(data) // 2]
    actual_size = imix if imix else IMIX_MIX
    avg_size = (sum(actual_size) / len(actual_size)) if isinstance(actual_size, list) else actual_size
    gbps_calc = median[0] * avg_size * 8 / 1000  # MPPS × bytes × 8 / 1000 = Gbps
    # Use reported Gbps from metrics (may differ due to actual protocol header mix)
    print(f"PPS={median[0]:.3f} MPPS  Gbps={median[1]:.3f}  n={len(data)}")
    return (median[0], median[1], len(data))


def main():
    print("=== 2b: Packet-Size Profile (batch=128, net_null) ===\n")
    print(f"{'Frame Size':>16}  {'MPPS':>8}  {'Gbps':>8}  {'PPS':>12}  {'Note'}")
    print("-" * 62)

    results = []
    for size in SIZES:
        if size is None:
            label = "IMIX mix"
            avg_size = sum(IMIX_MIX) / len(IMIX_MIX)
        else:
            label = f"{size}B"
            avg_size = size

        mpps, gbps, n = run_one(label, size if size else None)
        if mpps > 0:
            pps = int(mpps * 1_000_000)
            results.append((size, avg_size, mpps, gbps, pps, n))

    # Print summary
    print(f"\n{'Frame Size':>16}  {'MPPS':>8}  {'Gbps':>8}  {'PPS':>12}  {'Samples':>8}")
    print("-" * 62)
    for size, avg_size, mpps, gbps, pps, n in results:
        label = f"{size}B" if size else "IMIX mix"
        print(f"{label:>16}  {mpps:>8.3f}  {gbps:>8.3f}  {pps:>12,}  {n:>8}")

    # Find Gbps ceiling
    if results:
        best_gbps = max(results, key=lambda x: x[3])
        print(f"\nGbps ceiling: {best_gbps[3]:.3f} Gbps at {best_gbps[1]:.0f}-byte frames")


if __name__ == "__main__":
    main()
