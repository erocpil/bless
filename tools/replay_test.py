#!/usr/bin/env python3
"""Deterministic replay test — same seed => identical entropy.
Usage: python3 tools/replay_test.py [seed] [num]
"""
import json, subprocess, sys, time

BIN = "build/release-static/bin/bless"
CONFIG = "conf/config-ci.yaml"
SEED = int(sys.argv[1]) if len(sys.argv) > 1 else 42
NUM = int(sys.argv[2]) if len(sys.argv) > 2 else 50000
CURL = ["curl", "-s", "--noproxy", "*", "http://127.0.0.1:8000/api/stats"]


def poll(url, timeout=2):
    try:
        r = subprocess.run(url, capture_output=True, text=True, timeout=timeout)
        out = r.stdout.strip()
        if out and out != "{}":
            return json.loads(out)
    except Exception:
        pass
    return None


def run_round():
    proc = subprocess.Popen(
        [BIN, CONFIG, f"--seed={SEED}", f"--num={NUM}"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
    )
    last = None
    for _ in range(200):
        if proc.poll() is not None:
            break
        s = poll(CURL)
        if s:
            last = s
        time.sleep(0.3)
    proc.wait()
    return last


print(f"=== Round 1 (seed={SEED}, num={NUM}) ===")
r1 = run_round()
if not r1:
    print("FAIL: no stats captured for round 1")
    sys.exit(1)
e1 = r1["entropy"]
print(f"  opackets={r1['ports']['0']['stats']['opackets']} "
      f"protocol={e1['protocol']:.4f} src_ip={e1['src_ip']:.4f}")

time.sleep(2)

print(f"=== Round 2 (seed={SEED}, num={NUM}) ===")
r2 = run_round()
if not r2:
    print("FAIL: no stats captured for round 2")
    sys.exit(1)
e2 = r2["entropy"]
print(f"  opackets={r2['ports']['0']['stats']['opackets']} "
      f"protocol={e2['protocol']:.4f} src_ip={e2['src_ip']:.4f}")

# Compare
FIELDS = [
    "protocol", "src_ip", "dst_ip", "src_port", "dst_port",
    "pkt_size", "vxlan_encap", "outer_src_ip", "outer_dst_ip",
    "outer_src_port", "vni", "tcp_flags", "total_5tuple", "delta_tsc",
    "joint_5tuple",
    "mi_sip_dip", "mi_spt_dpt",
]
diffs = 0
print()
for f in FIELDS:
    v1 = e1.get(f)
    v2 = e2.get(f)
    ok = v1 == v2
    if not ok:
        print(f"  DIFF {f:20s}: {v1} != {v2}")
        diffs += 1

print(f"\nFields compared: {len(FIELDS)}, identical: {len(FIELDS) - diffs}, differ: {diffs}")
if diffs:
    print("FAIL: deterministic replay NOT verified")
    sys.exit(1)
else:
    print("PASS: deterministic replay verified — identical entropy")
