#!/usr/bin/env python3
"""
Quick single-run PPS + perf stat collector using matched measurement windows.

Same methodology as ladder_tier.py but single-run (no aggregation).
Use for spot-checks and iteration; use ladder_tier.py for benchmark-grade data.

Methodology:
  1. Launch bless in background
  2. Wait for stabilization
  3. Record pkt_start from /metrics
  4. Attach perf stat -p <pid> for WINDOW_SEC
  5. Record pkt_end from /metrics
  6. Kill bless
  7. Compute per-packet metrics from the SAME window
"""

import subprocess
import sys
import time
import re
import os

BLESS = "./build/release-static/bin/bless"
METRICS = "http://127.0.0.1:8000/metrics"
WINDOW_SEC = 10
STARTUP_TIMEOUT = 30
STABILIZE_SEC = 3

config = sys.argv[1]
label = sys.argv[2] if len(sys.argv) > 2 else config


def get_opackets():
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


# ── Launch ────────────────────────────────────────────────────────────
proc = subprocess.Popen(
    [BLESS, config],
    cwd="/root/src/bless",
    stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL,
)

# ── Wait for startup ──────────────────────────────────────────────────
for _ in range(STARTUP_TIMEOUT * 2):
    time.sleep(0.5)
    pkts = get_opackets()
    if pkts is not None and pkts > 100:
        break
else:
    print("ERR: startup timeout")
    proc.terminate()
    proc.wait()
    sys.exit(1)

# ── Stabilise ─────────────────────────────────────────────────────────
time.sleep(STABILIZE_SEC)
pkt_start = get_opackets()
if pkt_start is None:
    proc.terminate()
    proc.wait()
    print("ERR: /metrics unavailable at measurement start")
    sys.exit(1)

# ── Measure ───────────────────────────────────────────────────────────
perf_file = "/tmp/perf_result.txt"
run_start = time.time()
subprocess.run(
    [
        "perf", "stat",
        "-e", "cycles,instructions,branches,branch-misses",
        "-p", str(proc.pid),
        "--timeout", str(WINDOW_SEC * 1000),
        "-o", perf_file,
    ],
    stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL,
)
run_end = time.time()
actual_window = run_end - run_start

pkt_end = get_opackets()
if pkt_end is None:
    proc.terminate()
    proc.wait()
    print("ERR: /metrics unavailable at measurement end")
    sys.exit(1)

# ── Kill ──────────────────────────────────────────────────────────────
proc.terminate()
try:
    proc.wait(timeout=5)
except subprocess.TimeoutExpired:
    proc.kill()
    proc.wait()
time.sleep(0.5)

# ── Parse perf ────────────────────────────────────────────────────────
with open(perf_file) as f:
    perf_out = f.read()

def _extract(pat):
    m = re.search(pat, perf_out)
    return int(m.group(1).replace(",", "")) if m else 0

cycles = _extract(r"([\d,]+)\s+cycles\b")
instr  = _extract(r"([\d,]+)\s+instructions\b")
branches = _extract(r"([\d,]+)\s+branches\b")
bmiss   = _extract(r"([\d,]+)\s+branch-misses\b")

# ── Report ────────────────────────────────────────────────────────────
window_pkts = pkt_end - pkt_start
if window_pkts > 0:
    mpps   = window_pkts / 1e6 / actual_window
    cyc    = cycles / window_pkts
    inst   = instr / window_pkts
    br_pkt = branches / window_pkts
    bm_pkt = bmiss / window_pkts
    bm_pct = (bmiss / branches * 100) if branches else 0
    print(
        f"{label:25s}  {mpps:6.2f} MPPS  {cyc:7.0f} cyc/pkt  "
        f"{inst:7.0f} inst/pkt  br={br_pkt:.0f}/pkt  "
        f"bmiss={bm_pct:.1f}%  {actual_window:.1f}s  "
        f"{window_pkts:,d} pkts"
    )
else:
    print(f"{label:25s}  0 packets in window")
