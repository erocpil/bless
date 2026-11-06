#!/usr/bin/env python3
"""
Measure bless throughput and per-packet cost using matched measurement windows.

Methodology:
  1. Launch bless in the background.
  2. Wait for metrics endpoint + stable packet flow.
  3. Record pkt_start from /metrics.
  4. Attach `perf stat -p <pid>` for a fixed WINDOW_SEC interval.
  5. Record pkt_end from /metrics.
  6. Kill bless.
  7. Compute all per-packet metrics from the SAME window.

This avoids the prior mismatch where MPPS came from a 2-second tail window
while cycles/instructions covered the full process lifetime (initialisation,
warm-up, HTTP server, shutdown).

Current limitation: `perf stat -p <pid>` counts ALL threads (worker + sampler
control-plane + HTTP server), not just the TX worker hot-path.  This gives
*process-wide system cost per packet*.  For hot-path-only analysis, future
work should target the worker-thread TID (`perf stat -t <tid>`).

Reports: median (with min-max range) over 5 independent process restarts.
"""

import subprocess
import time
import re
import sys
import argparse
import statistics

import yaml

BLESS = "./build/release-static/bin/bless"
METRICS = "http://127.0.0.1:8000/metrics"
WINDOW_SEC = 10          # perf stat measurement window
STARTUP_TIMEOUT = 30     # max seconds to wait for /metrics
STABILIZE_SEC = 3        # wait after first packets before perf stat
RUNS = 5                 # independent process restarts per scenario
SLEEP_BETWEEN = 3        # cooldown between runs


def get_opackets():
    """Return cumulative opackets sum from /metrics, or None on failure."""
    try:
        out = subprocess.check_output(
            ["curl", "-s", "--noproxy", "*", METRICS],
            timeout=2, stderr=subprocess.DEVNULL,
        ).decode()
        return sum(int(x) for x in re.findall(
            r"dpdk_opackets\{[^}]*\}\s+(\d+)", out
        ))
    except Exception:
        return None


def run_one(config, label):
    """
    One independent run: start→stabilize→measure→stop.

    Returns dict or None on failure.
    """
    # ── Launch ────────────────────────────────────────────────────────
    perf_file = f"/tmp/perf_{label.replace('-', '_')}.txt"
    proc = subprocess.Popen(
        [BLESS, config],
        cwd="/root/src/bless",
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    # ── Wait for startup ──────────────────────────────────────────────
    for _ in range(STARTUP_TIMEOUT * 2):  # 0.5 s ticks
        time.sleep(0.5)
        pkts = get_opackets()
        if pkts is not None and pkts > 100:
            break
    else:
        proc.terminate()
        proc.wait()
        print(f"  ERR: startup timeout ({STARTUP_TIMEOUT}s)")
        return None

    # ── Stabilise ─────────────────────────────────────────────────────
    time.sleep(STABILIZE_SEC)
    pkt_start = get_opackets()
    if pkt_start is None:
        proc.terminate()
        proc.wait()
        print("  ERR: /metrics unavailable at measurement start")
        return None

    # ── Measure: perf stat -p <pid> for WINDOW_SEC ────────────────────
    run_start = time.time()
    perf_proc = subprocess.run(
        [
            "perf", "stat",
            "-e", "cycles,instructions,branches,branch-misses",
            "-p", str(proc.pid),
            "--timeout", str(WINDOW_SEC * 1000),  # ms
            "-o", perf_file,
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    run_end = time.time()
    actual_window = run_end - run_start

    # ── End-of-window counter ─────────────────────────────────────────
    pkt_end = get_opackets()
    if pkt_end is None:
        proc.terminate()
        proc.wait()
        print("  ERR: /metrics unavailable at measurement end")
        return None

    # ── Kill ──────────────────────────────────────────────────────────
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
    time.sleep(0.5)

    # ── Parse perf stat ───────────────────────────────────────────────
    try:
        with open(perf_file) as f:
            perf = f.read()

        def _extract(pat):
            m = re.search(pat, perf)
            if not m:
                raise ValueError(f"pattern not found: {pat}")
            return int(m.group(1).replace(",", "").replace(" ", ""))

        cycles = _extract(r"([\d,]+)\s+cycles\b")
        instr  = _extract(r"([\d,]+)\s+instructions\b")
        branches = _extract(r"([\d,]+)\s+branches\b")
        bmiss   = _extract(r"([\d,]+)\s+branch-misses\b")
        elapsed = float(re.search(
            r"([\d.]+)\s+seconds time elapsed", perf
        ).group(1))
    except Exception as e:
        print(f"  ERR parsing perf: {e}")
        return None

    window_pkts = pkt_end - pkt_start
    if window_pkts <= 0:
        print(f"  ERR: zero packets in window (start={pkt_start} end={pkt_end})")
        return None

    return {
        "label": label,
        "mpps": window_pkts / 1e6 / actual_window,
        "cyc_pkt": cycles / window_pkts,
        "inst_pkt": instr / window_pkts,
        "branches_pkt": branches / window_pkts,
        "bmiss_pkt": bmiss / window_pkts,
        "bmiss_rate": (bmiss / branches * 100) if branches else 0,
        "elapsed": elapsed,
        "actual_window_s": actual_window,
        "window_pkts": window_pkts,
        "cycles": cycles,
        "instr": instr,
        "branches": branches,
        "bmiss": bmiss,
        "pkt_start": pkt_start,
        "pkt_end": pkt_end,
        "pid": proc.pid,
    }


# ── Config diff (feature-isolation clean/cross-config check) ──────────
#
# Inner-config fields change the *shape* of the workload (protocol mix,
# address range, frame size, batch).  If any of these differ between two
# tiers, the delta is CROSS-CONFIG and must not be presented as a feature
# cost.  Feature fields are the composable toggles being isolated.

INNER_CONFIG_PREFIXES = (
    "injector.arp", "injector.icmp", "injector.tcp", "injector.udp",
    "injector.sctp", "injector.batch", "bless.ether.imix",
    "bless.ether.type.ipv4.src", "bless.ether.type.ipv4.dst",
    "bless.ether.type.ipv4.udp.", "bless.ether.type.ipv4.tcp.",
    "bless.ether.type.ipv4.sctp.", "bless.ether.type.ipv4.icmp.",
    "bless.ether.dst", "bless.ether.copy-payload", "bless.ether.mtu",
)

FEATURE_PREFIXES = (
    "bless.vxlan.", "bless.erroneous.", "bless.hw-offload",
    "injector.sample-interval",
)

IGNORED_PREFIXES = (
    "system.", "dpdk.", "injector.num", "injector.p", "injector.q",
    "injector.auto-start", "injector.mode", "injector.batch-delay-us",
    "injector.batch-jitter-us", "injector.latency-hist-enable",
    "injector.mi-smoothing-window", "injector.pps-rate",
)


def _flatten(d, prefix=""):
    """Flatten nested dict to dotted-path -> scalar/list."""
    out = {}
    for k, v in d.items():
        path = f"{prefix}.{k}" if prefix else str(k)
        if isinstance(v, dict):
            out.update(_flatten(v, path))
        else:
            out[path] = v
    return out


def _classify(path):
    for p in INNER_CONFIG_PREFIXES:
        if path == p or path.startswith(p):
            return "inner"
    for p in FEATURE_PREFIXES:
        if path == p or path.startswith(p):
            return "feature"
    for p in IGNORED_PREFIXES:
        if path == p or path.startswith(p):
            return "ignored"
    return "unknown"


def diff_configs(cfg_a, cfg_b):
    """Return (inner_diffs, feature_diffs, unknown_diffs, ignored_diffs).

    Each diff is a (path, val_a, val_b) tuple.
    """
    fa = _flatten(yaml.safe_load(open(cfg_a)))
    fb = _flatten(yaml.safe_load(open(cfg_b)))

    inner, feature, unknown, ignored = [], [], [], []
    for path in sorted(set(fa) | set(fb)):
        a = fa.get(path, "<missing>")
        b = fb.get(path, "<missing>")
        if a == b:
            continue
        cls = _classify(path)
        ({"inner": inner, "feature": feature,
          "unknown": unknown, "ignored": ignored})[cls].append((path, a, b))
    return inner, feature, unknown, ignored


def print_diff(cfg_a, cfg_b):
    """Print a clean/cross-config verdict for the two configs.  Returns the
    verdict string: 'clean', 'cross-config', or 'multi-feature'."""
    inner, feature, unknown, ignored = diff_configs(cfg_a, cfg_b)

    print(f"\n  ── CONFIG DIFF: {cfg_a}  →  {cfg_b} ──")
    for title, rows in (("INNER-CONFIG (workload shape)", inner),
                        ("FEATURE", feature),
                        ("UNCLASSIFIED (review manually)", unknown)):
        if not rows:
            continue
        print(f"    {title}:")
        for path, a, b in rows:
            print(f"      {path:50s} {a!r:>20s}  ->  {b!r}")

    if ignored:
        print(f"    (ignored {len(ignored)} env/runtime-only diffs)")

    # A single feature may expand into several sub-fields when enabled
    # (e.g. VXLAN -> enable + ratio + outer header).  Group feature diffs
    # by their top-level feature prefix before counting.
    feature_groups = set()
    for path, _a, _b in feature:
        for p in FEATURE_PREFIXES:
            if path == p or path.startswith(p):
                feature_groups.add(p)
                break

    if inner or unknown:
        verdict = "cross-config"
    elif len(feature_groups) == 1:
        verdict = "clean"
    elif len(feature_groups) > 1:
        verdict = "multi-feature"
    else:
        verdict = "clean"  # identical configs — nothing to isolate

    print(f"    VERDICT: {verdict}\n")
    return verdict


def main():
    ap = argparse.ArgumentParser(
        description="Measure bless per-packet cost with matched windows. "
                    "Optionally diff two configs to check clean/cross-config "
                    "before measuring."
    )
    ap.add_argument("config", help="bless config for this tier")
    ap.add_argument("label", help="tier label (e.g. T4)")
    ap.add_argument("--vs", metavar="BASELINE_CONFIG", default=None,
                    help="baseline config to diff against (clean/cross-config check)")
    ap.add_argument("--diff-only", action="store_true",
                    help="only print the config diff, do not run measurement")
    args = ap.parse_args()

    if args.vs:
        verdict = print_diff(args.vs, args.config)
        if args.diff_only:
            return 0

    config, label = args.config, args.label
    results = []

    for i in range(1, RUNS + 1):
        run_label = f"{label}-{i}"
        print(f"  Run {i}/{RUNS}...", end=" ", flush=True)
        r = run_one(config, run_label)
        if r:
            results.append(r)
            print(
                f"{r['mpps']:.2f} MPPS  "
                f"{r['cyc_pkt']:.0f} cyc/pkt  "
                f"{r['inst_pkt']:.0f} inst/pkt  "
                f"br={r['branches_pkt']:.0f}/pkt  "
                f"bmiss={r['bmiss_rate']:.1f}%"
            )
        else:
            print("FAILED")
        time.sleep(SLEEP_BETWEEN)

    if not results:
        print("All runs failed!")
        return 1

    # ── Aggregate ─────────────────────────────────────────────────────
    mpps   = [r["mpps"] for r in results]
    cyc    = [r["cyc_pkt"] for r in results]
    inst   = [r["inst_pkt"] for r in results]
    bmiss  = [r["bmiss_rate"] for r in results]

    def _report(name, values, fmt):
        med   = statistics.median(values)
        mn, mx = min(values), max(values)
        spread = mx - mn
        spread_pct = (spread / med * 100) if med else 0
        print(f"\n  {name}: median={fmt(med)}  [{fmt(mn)}–{fmt(mx)}]  "
              f"spread={spread_pct:.1f}%")

    print(f"\n{'='*65}")
    print(f"  AGGREGATE ({RUNS} independent runs)")
    print(f"{'='*65}")

    _report("MPPS",     mpps,  lambda v: f"{v:.2f}")
    _report("cyc/pkt",  cyc,   lambda v: f"{v:.0f}")
    _report("inst/pkt", inst,  lambda v: f"{v:.0f}")
    _report("bmiss%",   bmiss, lambda v: f"{v:.1f}")

    # Per-run detail
    print(f"\n  PER-RUN DATA:")
    print(f"  {'run':>4s}  {'MPPS':>7s}  {'cyc/pkt':>8s}  {'inst/pkt':>9s}  "
          f"{'br/pkt':>7s}  {'bmiss%':>7s}  {'window_s':>8s}  {'pkts':>15s}")
    for r in results:
        print(
            f"  {r['label']:>4s}  {r['mpps']:7.2f}  {r['cyc_pkt']:8.0f}  "
            f"{r['inst_pkt']:9.0f}  {r['branches_pkt']:7.0f}  "
            f"{r['bmiss_rate']:7.1f}  {r['actual_window_s']:8.1f}  "
            f"{r['window_pkts']:15,d}"
        )

    return 0


if __name__ == "__main__":
    sys.exit(main())
