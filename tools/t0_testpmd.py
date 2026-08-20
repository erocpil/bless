#!/usr/bin/env python3
"""
T0: testpmd txonly baseline with four-point bilateral error estimation.

Uses ``perf stat --delay=-1 --control=fifo:...`` and four telemetry
packet-counter reads bracketing the enable and disable boundaries:

  pkt_start_before → enable perf + wait ACK → pkt_start_after
       ↓                                          ↓
  [ pre-enable gap ]                    [ perf counter window ]
                                                               ↓
  pkt_end_before   → disable perf + wait ACK → pkt_end_after
       ↓                                          ↓
  [ perf counter window end ]           [ post-disable gap ]

The true number of packets sent during the perf counter window is
bounded by:

  pkts_lower = pkt_end_before − pkt_start_after   (conservative)
  pkts_upper = pkt_end_after  − pkt_start_before  (optimistic)

giving bilateral inst/pkt bounds that cover BOTH the pre-enable and
post-disable alignment gaps.

Protocol (5 independent runs):
  1.  Start testpmd with unique --file-prefix.
  2.  Wait for telemetry socket + verify liveness.
  3.  Send "start", wait for stabilisation.
  4.  Start perf stat --delay=-1 --control=fifo:... (counters disabled).
  5.  Wait until perf opens the control FIFO.
  6.  Read pkt_start_before from telemetry.
  7.  Enable perf counters (write "enable", read ACK via select).
  8.  Read pkt_start_after from telemetry.
  9.  Sleep WINDOW_SEC.
 10.  Read pkt_end_before from telemetry.
 11.  Disable perf counters (write "disable", read ACK via select).
 12.  Read pkt_end_after from telemetry.
 13.  Stop testpmd, clean up, compute bilateral bounds.
"""
import subprocess
import time
import re
import sys
import os
import select
import socket
import json
import statistics
import threading
from queue import Queue, Empty

TESTPMD       = "/opt/dpdk/bin/dpdk-testpmd"
WINDOW_SEC    = 10
STABILIZE_SEC = 3
RUNS          = 5
SLEEP_BETWEEN = 3

# Bless imix=[64] → 78‑byte L2 frame (14 Eth + 20 IP + 8 UDP + 36 payload).
# testpmd --txpkts=78 matches this so all tiers share the same L2 size.
TXPKTS_SIZE = 78


def _rmf(path):
    try:
        os.unlink(path)
    except OSError:
        pass


def query_opackets(sock_path):
    """Return opackets from telemetry, or None.  Print the exception so
    that misconfiguration is diagnosable."""
    try:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        sock.settimeout(3)
        sock.connect(sock_path)
        sock.recv(4096)  # init handshake
        sock.send(b"/ethdev/stats,0")
        data = sock.recv(16384)
        sock.close()
        result = json.loads(data.decode())
        return result["/ethdev/stats"]["opackets"]
    except ConnectionRefusedError:
        print(f"    telemetry: connection refused ({sock_path})")
        return None
    except PermissionError:
        print(f"    telemetry: permission denied ({sock_path})")
        return None
    except socket.timeout:
        print(f"    telemetry: timeout ({sock_path})")
        return None
    except json.JSONDecodeError as e:
        print(f"    telemetry: JSON decode error: {e}")
        return None
    except KeyError:
        print(f"    telemetry: unexpected response schema")
        return None
    except Exception as e:
        print(f"    telemetry: {type(e).__name__}: {e}")
        return None


def _write_ctl(fifo_path, cmd, timeout=5):
    """Write a command to the perf control FIFO.  Non-blocking open with
    a poll loop so we can detect if perf never opened the FIFO."""
    import errno as _errno
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            fd = os.open(fifo_path, os.O_WRONLY | os.O_NONBLOCK)
            break
        except OSError as e:
            if e.errno == _errno.ENXIO:
                time.sleep(0.05)
                continue
            raise
    else:
        raise TimeoutError(f"perf did not open {fifo_path} within {timeout}s")
    try:
        os.write(fd, cmd.encode())
    finally:
        os.close(fd)


def _read_ack(fifo_path, timeout=5):
    """Read one newline-terminated line from the ACK FIFO with a real
    timeout.  The fd is kept NONBLOCK; select() polls for readability.
    No blocking-mode switch — avoids the risk of permanent hang."""
    deadline = time.monotonic() + timeout
    fd = os.open(fifo_path, os.O_RDONLY | os.O_NONBLOCK)
    try:
        buf = bytearray()
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"no ACK from {fifo_path} within {timeout}s")
            readable, _, _ = select.select([fd], [], [], remaining)
            if not readable:
                raise TimeoutError(f"no ACK from {fifo_path} within {timeout}s")
            try:
                chunk = os.read(fd, 4096)
            except BlockingIOError:
                time.sleep(0.01)
                continue
            if not chunk:
                # Writer not yet connected — keep waiting
                time.sleep(0.01)
                continue
            buf.extend(chunk)
            if b"\n" in buf:
                return bytes(buf).split(b"\n", 1)[0].decode().strip()
    finally:
        os.close(fd)


def run_one(label, idx):
    prefix = f"t0_run_{idx}"
    telemetry_sock = f"/var/run/dpdk/{prefix}/dpdk_telemetry.v2"
    _rmf(telemetry_sock)

    fifo_path = f"/tmp/tpmd_stdin_{idx}"
    _rmf(fifo_path)
    os.mkfifo(fifo_path)

    ctl_fifo = f"/tmp/perf_ctl_{idx}.fifo"
    ack_fifo = f"/tmp/perf_ack_{idx}.fifo"
    _rmf(ctl_fifo); _rmf(ack_fifo)
    os.mkfifo(ctl_fifo)
    os.mkfifo(ack_fifo)

    # ── FIFO writer thread ──────────────────────────
    commands = Queue()
    writer_errors = Queue()

    def fifo_writer():
        try:
            with open(fifo_path, "w") as f:
                while True:
                    try:
                        cmd = commands.get(timeout=0.2)
                    except Empty:
                        continue
                    if cmd is None:
                        return
                    f.write(cmd)
                    f.flush()
        except Exception as e:
            writer_errors.put(e)

    tw = threading.Thread(target=fifo_writer, daemon=True)
    tw.start()

    # ── Start testpmd ──────────────────────────────
    try:
        fifo_r = open(fifo_path, "r")
    except OSError as e:
        commands.put(None); tw.join(timeout=2)
        _rmf(fifo_path)
        print(f"  ERR: cannot open FIFO for reading: {e}")
        return None

    try:
        proc = subprocess.Popen(
            [TESTPMD,
             "-l", "0-1", "-n", "2", "-a", "0000:00:04.0",
             "--file-prefix", prefix,
             "--",
             "--forward-mode=txonly",
             f"--txpkts={TXPKTS_SIZE}",
             "--burst=128",
             "--nb-cores=1", "--txq=1", "--rxq=0",
             "--port-topology=chained", "--stats-period=0",
             "-i"],
            cwd="/root/src/bless",
            stdin=fifo_r,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    finally:
        fifo_r.close()

    # ── Wait for telemetry ─────────────────────────
    deadline = time.monotonic() + 45
    while time.monotonic() < deadline:
        if os.path.exists(telemetry_sock):
            break
        time.sleep(0.2)
    else:
        proc.kill(); proc.wait()
        commands.put(None); tw.join(timeout=2)
        _rmf(fifo_path); _rmf(telemetry_sock)
        print(f"  ERR: telemetry socket not found ({telemetry_sock})")
        return None

    if query_opackets(telemetry_sock) is None:
        proc.kill(); proc.wait()
        commands.put(None); tw.join(timeout=2)
        _rmf(fifo_path)
        print(f"  ERR: telemetry socket unreachable")
        return None

    # ── Start TX ───────────────────────────────────
    commands.put("start\n")
    time.sleep(STABILIZE_SEC)

    # Check writer health early — if "start" failed we should not continue
    try:
        err = writer_errors.get_nowait()
        proc.kill(); proc.wait()
        commands.put(None); tw.join(timeout=2)
        print(f"  ERR: FIFO writer failed: {err}")
        return None
    except Empty:
        pass

    # ── Start perf (counters disabled) ─────────────
    perf_file = f"/tmp/perf_t0_{idx}.txt"
    perf = subprocess.Popen(
        ["perf", "stat",
         "-e", "cycles,instructions,branches,branch-misses",
         "-p", str(proc.pid),
         "--delay=-1",
         "--control", f"fifo:{ctl_fifo},{ack_fifo}",
         "--timeout", str((WINDOW_SEC + 10) * 1000),
         "-o", perf_file],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    # ── Wait for perf to open the control FIFO ─────
    try:
        _write_ctl(ctl_fifo, "", timeout=10)   # empty probe
    except TimeoutError:
        perf.kill(); perf.wait()
        proc.kill(); proc.wait()
        commands.put(None); tw.join(timeout=2)
        print("  ERR: perf did not open control FIFO")
        return None

    # ════════════════════════════════════════════════
    #  FOUR-POINT BILATERAL PROTOCOL
    # ════════════════════════════════════════════════

    # ── Point 1: pkt_start_before ──────────────────
    pkt_start_before = query_opackets(telemetry_sock)
    if pkt_start_before is None:
        perf.kill(); perf.wait()
        proc.kill(); proc.wait()
        commands.put(None); tw.join(timeout=2)
        print("  ERR: cannot read pkt_start_before")
        return None

    # ── Enable perf counters ───────────────────────
    try:
        _write_ctl(ctl_fifo, "enable\n")
        _read_ack(ack_fifo)
    except (TimeoutError, OSError) as e:
        perf.kill(); perf.wait()
        proc.kill(); proc.wait()
        commands.put(None); tw.join(timeout=2)
        print(f"  ERR: perf enable: {e}")
        return None
    ts_enable_ack = time.monotonic()

    # ── Point 2: pkt_start_after ───────────────────
    pkt_start_after = query_opackets(telemetry_sock)
    ts_start_after = time.monotonic()
    if pkt_start_after is None:
        perf.kill(); perf.wait()
        proc.kill(); proc.wait()
        commands.put(None); tw.join(timeout=2)
        print("  ERR: cannot read pkt_start_after")
        return None

    # ── Counted window ─────────────────────────────
    time.sleep(WINDOW_SEC)

    # ── Point 3: pkt_end_before ────────────────────
    pkt_end_before = query_opackets(telemetry_sock)
    ts_end_before = time.monotonic()
    if pkt_end_before is None:
        perf.kill(); perf.wait()
        proc.kill(); proc.wait()
        commands.put(None); tw.join(timeout=2)
        print("  ERR: cannot read pkt_end_before")
        return None

    # ── Disable perf counters ──────────────────────
    try:
        _write_ctl(ctl_fifo, "disable\n")
        _read_ack(ack_fifo)
    except (TimeoutError, OSError) as e:
        perf.kill(); perf.wait()
        proc.kill(); proc.wait()
        commands.put(None); tw.join(timeout=2)
        print(f"  ERR: perf disable: {e}")
        return None
    ts_disable_ack = time.monotonic()

    # ── Point 4: pkt_end_after ─────────────────────
    pkt_end_after = query_opackets(telemetry_sock)
    if pkt_end_after is None:
        # Non-fatal: use pkt_end_before for the "after" point too
        pkt_end_after = pkt_end_before

    # ── Stop testpmd, wait for perf ────────────────
    commands.put("stop\n")
    try:
        perf.wait(timeout=30)
    except subprocess.TimeoutExpired:
        perf.kill()
        perf.wait()
        print("  WARN: perf did not exit; killed")

    # Check writer health
    try:
        err = writer_errors.get_nowait()
        print(f"  WARN: FIFO writer error (stop): {err}")
    except Empty:
        pass

    commands.put("quit\n")
    time.sleep(0.5)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
    commands.put(None)
    tw.join(timeout=2)

    # Final writer error check
    try:
        err = writer_errors.get_nowait()
        print(f"  WARN: FIFO writer error (quit): {err}")
    except Empty:
        pass

    # Clean up
    _rmf(telemetry_sock)
    _rmf(fifo_path)
    _rmf(ctl_fifo)
    _rmf(ack_fifo)

    # ════════════════════════════════════════════════
    #  BILATERAL BOUNDS
    # ════════════════════════════════════════════════
    #
    #  The true number of packets sent during the perf
    #  counter window is between:
    #
    #    pkts_lower = pkt_end_before − pkt_start_after
    #      (pessimistic — excludes packets in both gaps)
    #
    #    pkts_upper = pkt_end_after  − pkt_start_before
    #      (optimistic — includes packets in both gaps)
    #
    #  Smaller denominator → higher per-packet cost.
    #  Larger denominator  → lower per-packet cost.
    #
    pkts_lower = pkt_end_before - pkt_start_after
    pkts_upper = pkt_end_after  - pkt_start_before

    if pkts_lower <= 0:
        print(f"  ERR: zero packets in lower bound "
              f"(start_after={pkt_start_after} end_before={pkt_end_before})")
        return None

    # Gap audit (for diagnosing uneven windows)
    head_pkts = pkt_start_after - pkt_start_before  # packets in pre-enable gap
    tail_pkts = pkt_end_after   - pkt_end_before    # packets in post-disable gap
    perf_head_s = ts_start_after - ts_enable_ack     # enable_ack → start_after read
    perf_tail_s = ts_disable_ack - ts_end_before     # end_before read → disable_ack

    # ── Parse perf stat ────────────────────────────
    try:
        with open(perf_file) as f:
            perf_text = f.read()
        def _extract(pat):
            m = re.search(pat, perf_text)
            return int(m.group(1).replace(",", "")) if m else 0
        cycles   = _extract(r"([\d,]+)\s+cycles\b")
        instr    = _extract(r"([\d,]+)\s+instructions\b")
        branches = _extract(r"([\d,]+)\s+branches\b")
        bmiss    = _extract(r"([\d,]+)\s+branch-misses\b")
    except Exception as e:
        print(f"  ERR parsing perf: {e}")
        return None

    # Bilateral inst/pkt:
    #   lower bound = instr / pkts_upper  (larger denominator)
    #   upper bound = instr / pkts_lower  (smaller denominator)
    inst_lower = instr / pkts_upper if pkts_upper else 0
    inst_upper = instr / pkts_lower if pkts_lower else 0
    inst_mid   = (inst_lower + inst_upper) / 2

    cyc_lower = cycles / pkts_upper if pkts_upper else 0
    cyc_upper = cycles / pkts_lower if pkts_lower else 0
    cyc_mid   = (cyc_lower + cyc_upper) / 2

    br_lower = branches / pkts_upper if pkts_upper else 0
    br_upper = branches / pkts_lower if pkts_lower else 0
    br_mid   = (br_lower + br_upper) / 2

    # Perf counter window (ACK→ACK)
    perf_window_s = ts_disable_ack - ts_enable_ack

    # IPC from per-run values (more correct than median(inst)/median(cyc))
    ipc = instr / cycles if cycles else 0

    # Spread of the bilateral bound as a percentage of midpoint
    bound_spread_pct = ((inst_upper - inst_lower) / inst_mid * 100) if inst_mid else 0

    return {
        "label": label,
        # Midpoint as primary metric; bounds for error bars
        "inst_pkt":       inst_mid,
        "inst_lower":     inst_lower,
        "inst_upper":     inst_upper,
        "cyc_pkt":        cyc_mid,
        "cyc_lower":      cyc_lower,
        "cyc_upper":      cyc_upper,
        "branches_pkt":   br_mid,
        "br_lower":       br_lower,
        "br_upper":       br_upper,
        "bmiss_rate":     (bmiss / branches * 100) if branches else 0,
        "ipc":            ipc,
        # Window audit
        "mpps_lower":     pkts_lower / 1e6 / perf_window_s,
        "mpps_upper":     pkts_upper / 1e6 / perf_window_s,
        "mpps_mid":       (pkts_lower + pkts_upper) / 2 / 1e6 / perf_window_s,
        "perf_window_s":  perf_window_s,
        "head_pkts":      head_pkts,
        "tail_pkts":      tail_pkts,
        "head_pkts_pct":  (head_pkts / pkts_lower * 100) if pkts_lower else 0,
        "tail_pkts_pct":  (tail_pkts / pkts_lower * 100) if pkts_lower else 0,
        "perf_head_s":    perf_head_s,
        "perf_tail_s":    perf_tail_s,
        "bound_spread_pct": bound_spread_pct,
    }


def main():
    for i in range(1, RUNS + 1):
        _rmf(f"/var/run/dpdk/t0_run_{i}/dpdk_telemetry.v2")
        _rmf(f"/tmp/tpmd_stdin_{i}")
        _rmf(f"/tmp/perf_ctl_{i}.fifo")
        _rmf(f"/tmp/perf_ack_{i}.fifo")

    results = []
    for i in range(1, RUNS + 1):
        run_label = f"T0-{i}"
        print(f"  Run {i}/{RUNS}...", end=" ", flush=True)
        r = run_one(run_label, i)
        if r:
            results.append(r)
            print(f"{r['mpps_mid']:.2f} MPPS  "
                  f"{r['inst_pkt']:.1f} inst/pkt "
                  f"[{r['inst_lower']:.1f}–{r['inst_upper']:.1f}]  "
                  f"cyc={r['cyc_pkt']:.1f}  br={r['branches_pkt']:.0f}  "
                  f"bmiss={r['bmiss_rate']:.1f}%  "
                  f"IPC={r['ipc']:.2f}  "
                  f"head={r['head_pkts_pct']:.2f}%  "
                  f"tail={r['tail_pkts_pct']:.2f}%  "
                  f"spread={r['bound_spread_pct']:.3f}%")
        else:
            print("FAILED")
        time.sleep(SLEEP_BETWEEN)

    if not results:
        print("\nAll runs failed!")
        return 1

    def _med(name, values, fmt):
        med = statistics.median(values)
        mn, mx = min(values), max(values)
        spread = (mx - mn) / med * 100 if med else 0
        print(f"  {name}: median={fmt(med)}  [{fmt(mn)}–{fmt(mx)}]  "
              f"spread={spread:.1f}%")

    print(f"\n{'='*78}")
    print(f"  T0 AGGREGATE ({RUNS} independent runs, four-point bilateral bounds)")
    print(f"{'='*78}")
    _med("MPPS (mid)",    [r["mpps_mid"]  for r in results], lambda v: f"{v:.2f}")
    _med("inst/pkt",      [r["inst_pkt"]  for r in results], lambda v: f"{v:.1f}")
    _med("inst lower",    [r["inst_lower"]for r in results], lambda v: f"{v:.1f}")
    _med("inst upper",    [r["inst_upper"]for r in results], lambda v: f"{v:.1f}")
    _med("cyc/pkt",       [r["cyc_pkt"]   for r in results], lambda v: f"{v:.1f}")
    _med("bmiss%",        [r["bmiss_rate"]for r in results], lambda v: f"{v:.1f}")
    _med("IPC",           [r["ipc"]       for r in results], lambda v: f"{v:.2f}")
    _med("head gap (pkts)",[r["head_pkts_pct"]for r in results], lambda v: f"{v:.2f}%")
    _med("tail gap (pkts)",[r["tail_pkts_pct"]for r in results], lambda v: f"{v:.2f}%")
    _med("bound spread",  [r["bound_spread_pct"] for r in results], lambda v: f"{v:.3f}%")

    print(f"\n  PER-RUN DATA:")
    hdr = (f"  {'label':>6s}  {'MPPS':>7s}  {'inst':>7s}  "
           f"{'bound':>15s}  {'cyc':>6s}  {'br':>5s}  "
           f"{'bmiss':>6s}  {'IPC':>5s}  "
           f"{'head%':>6s}  {'tail%':>6s}  {'spread%':>7s}")
    print(hdr)
    for r in results:
        print(f"  {r['label']:>6s}  {r['mpps_mid']:7.2f}  {r['inst_pkt']:6.1f}  "
              f"[{r['inst_lower']:5.1f}–{r['inst_upper']:5.1f}]  "
              f"{r['cyc_pkt']:5.1f}  {r['branches_pkt']:5.0f}  "
              f"{r['bmiss_rate']:5.1f}%  {r['ipc']:4.2f}  "
              f"{r['head_pkts_pct']:5.2f}%  {r['tail_pkts_pct']:5.2f}%  "
              f"{r['bound_spread_pct']:6.3f}%")

    return 0


if __name__ == "__main__":
    sys.exit(main())
