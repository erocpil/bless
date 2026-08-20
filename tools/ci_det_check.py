#!/usr/bin/env python3
"""Deterministic seed verification: compare entropy from two bless runs.

Usage: python3 tools/ci_det_check.py /tmp/run-a.json /tmp/run-b.json

Strips non-deterministic sections (meta, ports, log, PSD, observe) and
compares all remaining entropy/MI fields within EPS tolerance.  Ring-buffer
sampling races cause ~1e-5 bit variance between runs; this is inherent to
the asynchronous sampler architecture (not a bug).
"""
import json
import sys

EPS = 1e-3

def normalize(path: str) -> dict:
    with open(path) as f:
        d = json.load(f)
    ent = d.get('entropy', {})
    # Drop non-deterministic fields: max-entropy (sampling-window timing)
    # and timing entropy (TSC-delta based — hardware counter, not seed-determined)
    for k in list(ent.keys()):
        if k.startswith('mi_') and k.endswith('_max'):
            del ent[k]
        if k in ('delta_tsc', 'min_delta_tsc',
                 'mi_dtsc_proto', 'mi_dtsc_flow'):
            del ent[k]
    return ent

def main():
    if len(sys.argv) != 3:
        print(f'Usage: {sys.argv[0]} run-a.json run-b.json', file=sys.stderr)
        sys.exit(2)

    a = normalize(sys.argv[1])
    b = normalize(sys.argv[2])

    keys_a = set(a.keys())
    keys_b = set(b.keys())
    assert keys_a == keys_b, f'Key mismatch: {sorted(keys_a ^ keys_b)}'

    for k in sorted(keys_a):
        va, vb = a[k], b[k]
        if isinstance(va, (int, float)):
            diff = abs(va - vb)
            assert diff < EPS, f'{k}: {va} vs {vb} (diff={diff:.6f})'

    print(f'PASS: {len(keys_a)} entropy fields identical within epsilon ({EPS})')

if __name__ == '__main__':
    main()
