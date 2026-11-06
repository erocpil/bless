#!/usr/bin/env python3
"""2d: feature overhead matrix — measure PPS cost of each feature."""

import subprocess, time, urllib.request, sys, os, tempfile, yaml, copy

BATCH = 128
IMIX = [64]
SAMPLES = 5
SAMPLE_INTERVAL = 2
WARMUP = 5
MAX_RETRIES = 15
BENCH_YAML = "conf/config-bench.yaml"

# Override dicts — merged on top of config-bench.yaml
VXLAN_OVERRIDE = {
    "bless": {
        "vxlan": {
            "enable": True,
            "ratio": 10,
            "outer_ipv6": False,
            "wire_mtu": 0,
            "ether": {
                "src": "02:00:00:00:00:02",
                "dst": "02:00:00:00:00:03",
                "type": {
                    "ipv4": {
                        "src": "172.16.0.1+16",
                        "dst": ["10.0.0.1"],
                        "vni": [100],
                        "udp": {"src": "4789+1", "dst": "4789+1"},
                    }
                },
            },
        },
    },
}

ERRONEOUS_OVERRIDE = {
    "bless": {
        "erroneous": {
            "ratio": 102,
            "class": {
                "ipv4": ["version", "ihl", "cksum"],
                "tcp": ["syn_flood", "cksum"],
                "udp": ["len", "cksum"],
            },
        },
    },
}

ENTROPY_OVERRIDE = {
    "injector": {
        "sample-interval": 1,
        "entropy-target": 12,
        "entropy-dim": 8,
    },
}

CONFIGS = [
    ("Baseline", {}),
    ("+Entropy", ENTROPY_OVERRIDE),
    ("+VXLAN(10%)", VXLAN_OVERRIDE),
    ("+Erroneous(10%)", ERRONEOUS_OVERRIDE),
]


def deep_merge(base, override):
    """Recursively merge override dict into base dict."""
    result = copy.deepcopy(base)
    for k, v in override.items():
        if k in result and isinstance(result[k], dict) and isinstance(v, dict):
            result[k] = deep_merge(result[k], v)
        else:
            result[k] = v
    return result


def load_template():
    with open(BENCH_YAML) as f:
        return yaml.safe_load(f)


def collect_mpps(max_attempts, interval, want):
    results = []
    for _attempt in range(max_attempts):
        time.sleep(interval)
        try:
            with urllib.request.urlopen(
                "http://127.0.0.1:8000/metrics", timeout=5
            ) as resp:
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


def run_one(label, overrides):
    base = load_template()
    base["bless"]["ether"]["imix"] = IMIX
    cfg = deep_merge(base, overrides)

    sys.stdout.write(f"  {label:<20} ")
    sys.stdout.flush()

    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".yaml", delete=False
    ) as f:
        yaml.dump(cfg, f)
        tmp_path = f.name

    proc = subprocess.Popen(
        [
            "./build/release-static/bin/bless",
            tmp_path,
            "--",
            f"--batch={BATCH}",
            "-T",
            "1",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    time.sleep(WARMUP)

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
        return (0, 0)

    data.sort()
    median = data[len(data) // 2]
    print(f"{median:.3f} MPPS  n={len(data)}")
    return (median, len(data))


def main():
    print(
        "=== 2d: Feature Overhead Matrix "
        "(batch=128, IMIX=[64], net_null) ===\n"
    )

    # Build +All as merge of all three overrides
    all_override = deep_merge(deep_merge(ENTROPY_OVERRIDE, VXLAN_OVERRIDE),
                              ERRONEOUS_OVERRIDE)
    all_configs = CONFIGS + [("+All", all_override)]

    results = []
    for label, overrides in all_configs:
        mpps, nsamples = run_one(label, overrides)
        results.append((label, mpps, nsamples))

    if not results or results[0][1] == 0:
        print("NO RESULTS")
        return

    baseline_mpps = results[0][1]

    print(
        f"\n{'Config':<20}  {'MPPS':>8}  {'Δ MPPS':>8}"
        f"  {'% loss':>7}  {'PPS':>12}  {'Samples':>8}"
    )
    print("-" * 78)
    for label, mpps, nsamples in results:
        delta = mpps - baseline_mpps
        pct = (delta / baseline_mpps * 100) if baseline_mpps > 0 else 0
        pps = int(mpps * 1_000_000) if mpps > 0 else 0
        print(
            f"{label:<20}  {mpps:>8.3f}  {delta:>+8.3f}"
            f"  {pct:>+6.1f}%  {pps:>12,}  {nsamples:>8}"
        )

    print(
        f"\nBaseline: {baseline_mpps:.3f} MPPS "
        f"({int(baseline_mpps*1_000_000):,} PPS)"
    )

    if len(results) >= 5 and results[-1][1] > 0:
        total_loss = baseline_mpps - results[-1][1]
        indiv_sum = sum(abs(r[1] - baseline_mpps) for r in results[1:4])
        print(
            f"Combined loss: {total_loss:.3f} MPPS "
            f"({total_loss/baseline_mpps*100:.1f}%)"
        )
        print(f"Sum of individual losses: {indiv_sum:.3f} MPPS")
        if total_loss < indiv_sum:
            print(
                f"Synergy: combined loss ({total_loss:.3f})"
                f" < sum of parts ({indiv_sum:.3f})"
            )


if __name__ == "__main__":
    main()
