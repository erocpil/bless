#!/usr/bin/env python3
"""Repeatable observability/CPU validation with raw, auditable samples.

The dashboard ``bless_cpu_busy_pct`` value is retained for comparison, but CPU
cost is derived independently from per-thread /proc CPU time:

    cores_used = sum(delta thread CPU seconds) / delta wall seconds

Each scenario is run in a fresh Bless process.  Results are written below a
timestamped output directory as raw JSONL samples, per-run JSON, and summary
CSV.  This script does not require perf or pidstat.
"""

import argparse
import copy
import csv
import datetime as dt
import json
import os
from pathlib import Path
import signal
import statistics
import subprocess
import sys
import tempfile
import time
import urllib.request

try:
    import yaml
except ImportError:
    sys.exit("ERROR: PyYAML is required (python3-yaml on Debian/Ubuntu)")


SCENARIOS = (
    ("paced_sampler_off", 1000, 0),
    ("paced_sampler_10", 1000, 10),
    ("unlimited_sampler_off", 0, 0),
    ("unlimited_sampler_10", 0, 10),
)


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", default="./build/release-static/bin/bless")
    parser.add_argument("--config", default="conf/config-test.yaml")
    parser.add_argument("--url", default="http://127.0.0.1:8000/api/stats")
    parser.add_argument("--restarts", type=int, default=5)
    parser.add_argument("--warmup", type=float, default=5.0)
    parser.add_argument("--duration", type=float, default=20.0)
    parser.add_argument("--interval", type=float, default=1.0)
    parser.add_argument("--output", help="output directory (default: timestamped)")
    args = parser.parse_args()
    if args.restarts < 1 or args.warmup < 0 or args.duration <= 0:
        parser.error("restarts and duration must be positive; warmup must be non-negative")
    if args.interval <= 0 or args.interval > args.duration:
        parser.error("interval must be positive and no greater than duration")
    return args


def fetch_json(url):
    request = urllib.request.Request(url, headers={"Accept": "application/json"})
    with urllib.request.urlopen(request, timeout=2) as response:
        return json.load(response)


def thread_ticks(pid):
    """Return {tid: (comm, user+system ticks)} for the process."""
    result = {}
    for stat_path in Path(f"/proc/{pid}/task").glob("*/stat"):
        try:
            text = stat_path.read_text()
            right_paren = text.rfind(")")
            comm = text[text.find("(") + 1:right_paren]
            fields = text[right_paren + 2:].split()
            # fields starts at proc stat field 3; utime/stime are fields 14/15.
            ticks = int(fields[11]) + int(fields[12])
            result[int(stat_path.parent.name)] = (comm, ticks)
        except (FileNotFoundError, ProcessLookupError):
            continue
    return result


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    index = (len(ordered) - 1) * fraction
    lower = int(index)
    upper = min(lower + 1, len(ordered) - 1)
    weight = index - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def median_or_none(values):
    return statistics.median(values) if values else None


def configure(template, delay_us, sample_interval):
    config = copy.deepcopy(template)
    injector = config.setdefault("injector", {})
    injector["batch-delay-us"] = delay_us
    injector["batch-jitter-us"] = 5 if delay_us else 0
    injector["sample-interval"] = sample_interval
    injector["num"] = -1
    injector["auto-start"] = True
    return config


def stop_process(proc):
    if proc.poll() is not None:
        return
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


def wait_ready(proc, url, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"Bless exited during startup with status {proc.returncode}")
        try:
            fetch_json(url)
            return
        except Exception:
            time.sleep(0.25)
    raise RuntimeError("statistics API did not become ready within 30 seconds")


def run_once(args, output, template, scenario, restart):
    label, delay_us, sample_interval = scenario
    run_name = f"{label}-r{restart:02d}"
    log_path = output / f"{run_name}.log"
    raw_path = output / f"{run_name}.jsonl"
    result_path = output / f"{run_name}.json"
    config = configure(template, delay_us, sample_interval)

    with tempfile.NamedTemporaryFile(mode="w", suffix=".yaml", delete=False) as cfg_file:
        yaml.safe_dump(config, cfg_file, sort_keys=False)
        config_path = cfg_file.name

    proc = None
    try:
        with log_path.open("w") as log_file:
            proc = subprocess.Popen(
                [args.bin, config_path], stdout=log_file, stderr=subprocess.STDOUT,
            )
        wait_ready(proc, args.url)
        time.sleep(args.warmup)

        clk_tck = os.sysconf("SC_CLK_TCK")
        start_wall = time.monotonic()
        start_ticks = thread_ticks(proc.pid)
        samples = []
        next_sample = start_wall
        deadline = start_wall + args.duration
        with raw_path.open("w") as raw_file:
            while time.monotonic() < deadline:
                next_sample += args.interval
                try:
                    payload = fetch_json(args.url)
                    observed = payload.get("observe", {})
                    entropy = payload.get("entropy", {})
                    sample = {
                        "monotonic_s": time.monotonic(),
                        "tx_mpps": observed.get("tx_mpps"),
                        "process_cpu_cores": observed.get("process_cpu_cores"),
                        "enabled_lcores": observed.get("enabled_lcores"),
                        "enabled_lcore_utilization_ratio": observed.get(
                            "enabled_lcore_utilization_ratio"
                        ),
                        "reported_cpu_busy_pct": observed.get("cpu_busy_pct"),
                        "mem_rss_kb": observed.get("mem_rss_kb"),
                        "sampler_overwritten": entropy.get("sampler_overwritten"),
                        "sampler_overwritten_window": entropy.get("sampler_overwritten_window"),
                        "tx_submit_overshoot_p99_us": observed.get("tx_submit_overshoot_p99_us"),
                        "tx_burst_duration_p99_us": observed.get("tx_burst_duration_p99_us"),
                    }
                    samples.append(sample)
                    raw_file.write(json.dumps(sample, sort_keys=True) + "\n")
                    raw_file.flush()
                except Exception as error:
                    raw_file.write(json.dumps({"error": str(error)}) + "\n")
                time.sleep(max(0.0, next_sample - time.monotonic()))

        end_wall = time.monotonic()
        end_ticks = thread_ticks(proc.pid)
        wall_seconds = end_wall - start_wall
        per_thread = []
        total_delta_ticks = 0
        for tid, (comm, final_ticks) in sorted(end_ticks.items()):
            initial = start_ticks.get(tid)
            if initial is None:
                continue
            delta_ticks = max(0, final_ticks - initial[1])
            total_delta_ticks += delta_ticks
            per_thread.append({
                "tid": tid,
                "comm": comm,
                "cpu_seconds": delta_ticks / clk_tck,
                "cpu_cores_used": delta_ticks / clk_tck / wall_seconds,
            })

        numeric = lambda key: [
            float(sample[key]) for sample in samples
            if isinstance(sample.get(key), (int, float))
        ]
        tx_mpps = numeric("tx_mpps")
        reported_cpu = numeric("reported_cpu_busy_pct")
        metric_cpu_cores = numeric("process_cpu_cores")
        metric_cpu_ratio = numeric("enabled_lcore_utilization_ratio")
        rss = numeric("mem_rss_kb")
        overwrite_window = numeric("sampler_overwritten_window")
        result = {
            "scenario": label,
            "restart": restart,
            "batch_delay_us": delay_us,
            "sample_interval": sample_interval,
            "wall_seconds": wall_seconds,
            "sample_count": len(samples),
            "process_cpu_seconds": total_delta_ticks / clk_tck,
            "process_cpu_cores_used": total_delta_ticks / clk_tck / wall_seconds,
            "metric_process_cpu_cores_p50": median_or_none(metric_cpu_cores),
            "metric_enabled_lcore_utilization_ratio_p50": median_or_none(
                metric_cpu_ratio
            ),
            "tx_mpps_p50": median_or_none(tx_mpps),
            "tx_mpps_p95": percentile(tx_mpps, 0.95),
            "reported_cpu_busy_pct_p50": median_or_none(reported_cpu),
            "mem_rss_kb_p50": median_or_none(rss),
            "sampler_overwritten_window_max": max(overwrite_window, default=None),
            "threads": per_thread,
            "log": str(log_path),
            "raw_samples": str(raw_path),
        }
        result_path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
        return result
    finally:
        if proc is not None:
            stop_process(proc)
        os.unlink(config_path)


def write_summary(output, results):
    fields = [
        "scenario", "restart", "batch_delay_us", "sample_interval",
        "wall_seconds", "sample_count", "process_cpu_seconds",
        "process_cpu_cores_used", "tx_mpps_p50", "tx_mpps_p95",
        "metric_process_cpu_cores_p50",
        "metric_enabled_lcore_utilization_ratio_p50",
        "reported_cpu_busy_pct_p50", "mem_rss_kb_p50",
        "sampler_overwritten_window_max",
    ]
    with (output / "runs.csv").open("w", newline="") as csv_file:
        writer = csv.DictWriter(csv_file, fieldnames=fields)
        writer.writeheader()
        for result in results:
            writer.writerow({field: result.get(field) for field in fields})

    summaries = []
    for scenario, _, _ in SCENARIOS:
        runs = [result for result in results if result["scenario"] == scenario]
        summaries.append({
            "scenario": scenario,
            "restarts": len(runs),
            "tx_mpps_between_run_p50": median_or_none([
                run["tx_mpps_p50"] for run in runs if run["tx_mpps_p50"] is not None
            ]),
            "process_cpu_cores_between_run_p50": median_or_none([
                run["process_cpu_cores_used"] for run in runs
            ]),
            "process_cpu_cores_min": min(
                (run["process_cpu_cores_used"] for run in runs), default=None
            ),
            "process_cpu_cores_max": max(
                (run["process_cpu_cores_used"] for run in runs), default=None
            ),
        })
    (output / "summary.json").write_text(
        json.dumps(summaries, indent=2, sort_keys=True) + "\n"
    )


def main():
    args = parse_args()
    output = Path(args.output) if args.output else Path(
        "results/observability-cpu-" + dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    )
    output.mkdir(parents=True, exist_ok=False)
    with open(args.config) as config_file:
        template = yaml.safe_load(config_file)

    metadata = {
        "command": sys.argv,
        "binary": str(Path(args.bin).resolve()),
        "config": str(Path(args.config).resolve()),
        "git_commit": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], text=True
        ).strip(),
        "started_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "restarts": args.restarts,
        "warmup_seconds": args.warmup,
        "duration_seconds": args.duration,
        "interval_seconds": args.interval,
        "cpu_count": os.cpu_count(),
        "clock_ticks_per_second": os.sysconf("SC_CLK_TCK"),
    }
    (output / "metadata.json").write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n"
    )

    results = []
    try:
        # Interleave scenarios by restart so temperature and host-load drift do
        # not affect all repetitions of one scenario in the same direction.
        for restart in range(1, args.restarts + 1):
            for scenario in SCENARIOS:
                print(f"running {scenario[0]} restart {restart}/{args.restarts}", flush=True)
                result = run_once(args, output, template, scenario, restart)
                results.append(result)
                mpps = result["tx_mpps_p50"]
                mpps_text = f"{mpps:.6f}" if mpps is not None else "unavailable"
                print(f"  {mpps_text} MPPS, "
                      f"{result['process_cpu_cores_used']:.3f} CPU cores",
                      flush=True)
    finally:
        write_summary(output, results)
    print(f"results: {output}")


if __name__ == "__main__":
    main()
