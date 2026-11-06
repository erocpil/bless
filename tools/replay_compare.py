#!/usr/bin/env python3
"""Compare two bless /api/stats JSON snapshots for deterministic replay.

Extracts all entropy-related fields and reports differences.
Exit 0 if identical, 1 if any field differs.
"""
import json
import sys

ENTROPY_FIELDS = [
    "protocol", "src_ip", "dst_ip", "src_port", "dst_port",
    "pkt_size", "vxlan_encap", "outer_src_ip", "outer_dst_ip",
    "outer_src_port", "vni", "tcp_flags", "total_5tuple", "delta_tsc",
    "flow_distinct", "flow_total", "flow_ratio",
    "joint_5tuple",
]

MI_FIELDS = [
    "mi_sip_dip", "mi_spt_dpt", "mi_proto_spt",
    "mi_size_dpt", "mi_size_proto", "mi_dtsc_proto", "mi_dtsc_flow",
    "mi_tcpf_sz", "mi_tcpf_spt", "mi_tcpf_dpt",
    "mi_osip_odip", "mi_vni_osip",
]

PSD_FIELDS = ["dominant_hz", "spectral_flatness"]


def load_json(path):
    with open(path) as f:
        raw = f.read().strip()
    if not raw:
        print("ERROR: empty JSON file")
        sys.exit(1)
    return json.loads(raw)


def compare_nested(d1, d2, fields, label):
    diffs = []
    for f in fields:
        v1 = d1.get(f)
        v2 = d2.get(f)
        if v1 != v2:
            diffs.append((f, v1, v2))
    total = len(fields)
    matched = total - len(diffs)
    print(f"{label}: {matched}/{total} identical", end="")
    if diffs:
        print(f" — {len(diffs)} differ")
        for f, v1, v2 in diffs:
            print(f"  {f}: {v1} != {v2}")
    else:
        print()
    return diffs


def main():
    a = load_json(sys.argv[1])
    b = load_json(sys.argv[2])

    all_diffs = 0

    entropy = a.get("entropy", {})
    entropy2 = b.get("entropy", {})
    all_diffs += len(compare_nested(entropy, entropy2, ENTROPY_FIELDS, "Shannon entropy"))
    all_diffs += len(compare_nested(entropy, entropy2, MI_FIELDS, "Mutual info"))
    all_diffs += len(compare_nested(entropy, entropy2, ["min_protocol", "min_src_ip",
        "min_dst_ip", "min_src_port", "min_dst_port", "min_pkt_size"], "Min-entropy"))

    psd = a.get("psd", {})
    psd2 = b.get("psd", {})
    all_diffs += len(compare_nested(psd, psd2, PSD_FIELDS, "PSD"))

    # Compare tx counters
    ports = a.get("ports", {})
    ports2 = b.get("ports", {})
    for pid in ports:
        if pid in ports2:
            s1 = ports[pid].get("stats", {})
            s2 = ports2[pid].get("stats", {})
            for key in ["opackets"]:
                if s1.get(key) != s2.get(key):
                    all_diffs += 1
                    print(f"  port {pid} {key}: {s1.get(key)} != {s2.get(key)}")

    print(f"\nTotal fields with differences: {all_diffs}")
    if all_diffs:
        print("FAIL: deterministic replay NOT verified")
        sys.exit(1)
    else:
        print("PASS: deterministic replay verified — identical entropy metrics")


if __name__ == "__main__":
    main()
