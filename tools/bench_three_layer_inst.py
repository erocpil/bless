#!/usr/bin/env python3
"""Three-layer inst/pkt comparison: net_null vs ConnectX-6 PF vs VF.

Matched-window perf stat on the TX worker thread (busy-poll, bound to
lcore 1 = CPU 1).  inst/pkt = window instructions / (pkt_end - pkt_start).
5 restarts per layer, interleaved ABCABC to cancel environmental drift.

Primary metric:  inst/pkt (per-packet software cost).
Secondary:       IPC (inst/cycle), branch/pkt, branch-miss/pkt.
"""

import subprocess
import time
import urllib.request
import statistics
import os

BLESS = "/root/src/bless/build/release-static/bin/bless"
URL = "http://127.0.0.1:8000/metrics"

CONFIGS = [
    ("T-soft", "/root/src/bless/conf/bench-3layer-soft.yaml"),
    ("T-pf",   "/root/src/bless/conf/bench-3layer-pf.yaml"),
    ("T-vf",   "/root/src/bless/conf/bench-3layer-vf.yaml"),
]

WARMUP = 5
WINDOW = 10          # perf stat window, seconds
RESTARTS = 5
PERF_EVENTS = "instructions,cycles,branches,branch-misses"


def fetch_opackets():
    try:
        with urllib.request.urlopen(URL, timeout=3) as r:
            text = r.read().decode()
    except Exception:
        return None
    for line in text.splitlines():
        if line.startswith("dpdk_opackets"):
            try:
                return int(line.split()[1])
            except (ValueError, IndexError):
                return None
    return None


def find_worker_tid(pid):
    """Return the TX worker thread TID (bound to lcore 1 = CPU 1)."""
    task_dir = f"/proc/{pid}/task"
    # Primary: Cpus_allowed_list == "1" (worker pinned to CPU 1).
    for tid in os.listdir(task_dir):
        try:
            with open(f"{task_dir}/{tid}/status") as f:
                for line in f:
                    if line.startswith("Cpus_allowed_list:"):
                        cpus = line.split(":", 1)[1].strip()
                        if cpus == "1":
                            return int(tid)
        except (OSError, ValueError):
            pass
    # Fallback: largest utime+stime delta over 1s (busy-poll thread).
    def cpu_ticks():
        out = {}
        for tid in os.listdir(task_dir):
            try:
                with open(f"{task_dir}/{tid}/stat") as f:
                    p = f.read().split()
                out[int(tid)] = int(p[13]) + int(p[14])
            except (ValueError, IndexError, OSError):
                pass
        return out
    t1 = cpu_ticks()
    time.sleep(1.0)
    t2 = cpu_ticks()
    best_tid, best_delta = None, -1
    for tid, v2 in t2.items():
        d = v2 - t1.get(tid, 0)
        if d > best_delta:
            best_delta, best_tid = d, tid
    return best_tid


def parse_perf(path, field):
    try:
        with open(path) as f:
            for line in f:
                if field in line and not line.lstrip().startswith("#"):
                    return float(line.split()[0].replace(",", ""))
    except OSError:
        pass
    return None


def run_once(label, cfg, round_no):
    log = f"/tmp/bench_inst_{label}_r{round_no}.log"
    lf = open(log, "w")
    proc = subprocess.Popen([BLESS, cfg, "--", "-T", "1"],
                            stdout=lf, stderr=lf)
    time.sleep(WARMUP)
    if proc.poll() is not None:
        lf.close()
        print(f"  {label:<8} EXITED EARLY rc={proc.returncode}", flush=True)
        return None

    tid = find_worker_tid(proc.pid)
    if tid is None:
        lf.close()
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill(); proc.wait()
        print(f"  {label:<8} NO WORKER TID", flush=True)
        return None

    pkt_start = fetch_opackets()
    perf_path = f"/tmp/perf_inst_{label}_r{round_no}.txt"
    subprocess.run(
        ["perf", "stat", "-e", PERF_EVENTS, "-t", str(tid),
         "--timeout", str(WINDOW * 1000), "-o", perf_path],
        capture_output=True)
    pkt_end = fetch_opackets()

    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill(); proc.wait()
    lf.close()

    if pkt_start is None or pkt_end is None or pkt_end <= pkt_start:
        return None

    inst = parse_perf(perf_path, "instructions")
    cycles = parse_perf(perf_path, "cycles")
    branches = parse_perf(perf_path, "branches")
    bmiss = parse_perf(perf_path, "branch-misses")

    if not inst:
        return None

    pkts = pkt_end - pkt_start
    return {
        "inst_pkt": inst / pkts,
        "ipc": (inst / cycles) if cycles else None,
        "branch_pkt": (branches / pkts) if branches else None,
        "bmiss_pkt": (bmiss / pkts) if bmiss is not None else None,
        "pkts": pkts,
        "tid": tid,
    }


def main():
    results = {c[0]: [] for c in CONFIGS}
    for r in range(1, RESTARTS + 1):
        print(f"--- round {r}/{RESTARTS} ---", flush=True)
        for label, cfg in CONFIGS:
            res = run_once(label, cfg, r)
            if res is None:
                print(f"  {label:<8} FAILED", flush=True)
                results[label].append(None)
                continue
            results[label].append(res)
            print(f"  {label:<8} inst/pkt={res['inst_pkt']:.1f} "
                  f"IPC={res['ipc']:.2f} branch/pkt={res['branch_pkt']:.1f} "
                  f"bmiss/pkt={res['bmiss_pkt']:.2f} "
                  f"(tid={res['tid']}, {res['pkts']/1e6:.1f}M pkt)",
                  flush=True)
        time.sleep(1)

    print("\n===== SUMMARY (median across rounds) =====", flush=True)
    medians = {}
    for label, _cfg in CONFIGS:
        vals = [v for v in results[label] if v is not None]
        if not vals:
            print(f"{label:<8} NO DATA", flush=True)
            continue
        medians[label] = {
            "inst_pkt": statistics.median([v["inst_pkt"] for v in vals]),
            "ipc": statistics.median([v["ipc"] for v in vals]),
            "branch_pkt": statistics.median([v["branch_pkt"] for v in vals]),
            "bmiss_pkt": statistics.median([v["bmiss_pkt"] for v in vals]),
        }
        m = medians[label]
        print(f"{label:<8} inst/pkt={m['inst_pkt']:.1f} IPC={m['ipc']:.2f} "
              f"branch/pkt={m['branch_pkt']:.1f} "
              f"bmiss/pkt={m['bmiss_pkt']:.2f} (n={len(vals)})", flush=True)

    s = medians.get("T-soft")
    p = medians.get("T-pf")
    v = medians.get("T-vf")
    if s and p and v:
        print("\n===== DELTAS (inst/pkt) =====", flush=True)
        print(f"  T-pf - T-soft = {p['inst_pkt'] - s['inst_pkt']:+.1f}  "
              f"({(p['inst_pkt']/s['inst_pkt']-1)*100:+.1f}%)", flush=True)
        print(f"  T-vf - T-pf   = {v['inst_pkt'] - p['inst_pkt']:+.1f}  "
              f"({(v['inst_pkt']/p['inst_pkt']-1)*100:+.1f}%)", flush=True)


if __name__ == "__main__":
    main()
