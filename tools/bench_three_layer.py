#!/usr/bin/env python3
"""Three-layer TX throughput comparison: net_null vs ConnectX-6 PF vs VF.

Unified inner config (UDP 64B, batch=128, single worker,
copy-payload=false, sample-interval=0).  Only the dpdk section differs
across layers.  5 independent restarts per layer, interleaved ABCABC...
to cancel environmental drift.

Primary metric:  dpdk_opackets cumulative-counter delta rate (MPPS).
All three layers expose a non-zero opackets counter (net_null included,
DPDK 23.11.3 ethdev counts TX submissions even for net_null).
Cross-check: bless_tx_mpps (software counter, instantaneous rate).
"""

import subprocess
import time
import urllib.request
import statistics

BLESS = "/root/src/bless/build/release-static/bin/bless"
URL = "http://127.0.0.1:8000/metrics"

CONFIGS = [
    ("T-soft", "/root/src/bless/conf/bench-3layer-soft.yaml"),
    ("T-pf",   "/root/src/bless/conf/bench-3layer-pf.yaml"),
    ("T-vf",   "/root/src/bless/conf/bench-3layer-vf.yaml"),
]

WARMUP = 5
SAMPLE_INTERVAL = 2
SAMPLES = 5
RESTARTS = 5


def fetch():
    try:
        with urllib.request.urlopen(URL, timeout=3) as r:
            text = r.read().decode()
    except Exception:
        return None, None
    mpps = opackets = None
    for line in text.splitlines():
        if line.startswith("bless_tx_mpps "):
            try:
                mpps = float(line.split()[1])
            except (ValueError, IndexError):
                pass
        elif line.startswith("dpdk_opackets"):
            try:
                opackets = int(line.split()[1])
            except (ValueError, IndexError):
                pass
    return mpps, opackets


def clear_residual():
    subprocess.run(["pkill", "-x", "bless"], capture_output=True)
    time.sleep(0.5)


def run_once(label, cfg, round_no):
    log = f"/tmp/bench_3layer_{label}_r{round_no}.log"
    lf = open(log, "w")
    cmd = [BLESS, cfg, "--", "-T", "1"]
    proc = subprocess.Popen(cmd, stdout=lf, stderr=lf)
    time.sleep(WARMUP)

    if proc.poll() is not None:
        lf.close()
        print(f"  {label:<8} EXITED EARLY rc={proc.returncode} (see {log})",
              flush=True)
        return None

    mpps_samples = []
    op_samples = []
    for _ in range(SAMPLES):
        time.sleep(SAMPLE_INTERVAL)
        m, op = fetch()
        if m and m > 0:
            mpps_samples.append(m)
        if op is not None and op > 0:
            op_samples.append(op)

    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
    lf.close()

    if len(op_samples) < 2:
        return None

    # per-interval delta rates, then median (robust to one bad sample)
    rates = [(op_samples[i + 1] - op_samples[i]) / SAMPLE_INTERVAL / 1e6
             for i in range(len(op_samples) - 1)]
    op_rate = statistics.median(rates)
    mpps_med = (statistics.median(mpps_samples)
                if mpps_samples else None)
    return {"op_rate": op_rate, "mpps_med": mpps_med}


def main():
    clear_residual()
    op_results = {c[0]: [] for c in CONFIGS}
    mpps_results = {c[0]: [] for c in CONFIGS}

    for r in range(1, RESTARTS + 1):
        print(f"--- round {r}/{RESTARTS} ---", flush=True)
        for label, cfg in CONFIGS:
            clear_residual()
            res = run_once(label, cfg, r)
            if res is None:
                print(f"  {label:<8} FAILED", flush=True)
                op_results[label].append(None)
                continue
            op_results[label].append(res["op_rate"])
            if res["mpps_med"] is not None:
                mpps_results[label].append(res["mpps_med"])
            mstr = (f"{res['mpps_med']:.3f}"
                    if res["mpps_med"] is not None else "n/a")
            print(f"  {label:<8} opackets={res['op_rate']:.3f} MPPS  "
                  f"bless_tx_mpps={mstr}", flush=True)
        time.sleep(1)

    print("\n===== SUMMARY (median of per-round opackets rates) =====",
          flush=True)
    print(f"{'layer':<10} {'MPPS median':>12} {'min':>10} {'max':>10} "
          f"{'rounds':>7}", flush=True)
    medians = {}
    for label, _cfg in CONFIGS:
        vals = [v for v in op_results[label] if v is not None]
        if not vals:
            print(f"{label:<10} NO DATA", flush=True)
            continue
        med = statistics.median(vals)
        medians[label] = med
        print(f"{label:<10} {med:>12.3f} {min(vals):>10.3f} "
              f"{max(vals):>10.3f} {len(vals):>7}", flush=True)

    soft = medians.get("T-soft")
    pf = medians.get("T-pf")
    vf = medians.get("T-vf")
    print("\n===== DELTAS =====", flush=True)
    if soft and pf:
        print(f"  T-pf - T-soft (hardware TX path)  = {pf - soft:+.3f} MPPS  "
              f"({(pf / soft - 1) * 100:+.1f}%)", flush=True)
    if pf and vf:
        print(f"  T-vf - T-pf   (SR-IOV + e-switch) = {vf - pf:+.3f} MPPS  "
              f"({(vf / pf - 1) * 100:+.1f}%)", flush=True)
    if soft and vf:
        print(f"  T-vf - T-soft (full HW+virtual)   = {vf - soft:+.3f} MPPS  "
              f"({(vf / soft - 1) * 100:+.1f}%)", flush=True)

    print("\n===== bless_tx_mpps cross-check (software counter) =====",
          flush=True)
    for label, _cfg in CONFIGS:
        if mpps_results[label]:
            print(f"  {label:<10} {statistics.median(mpps_results[label]):.3f} "
                  f"MPPS (n={len(mpps_results[label])})", flush=True)


if __name__ == "__main__":
    main()
