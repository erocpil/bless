# Dual-Endpoint Handshake and Observe Experiment

This experiment validates Bless's bidirectional TCP handshake engine and the
Observe dashboard before introducing a physical DUT. Two independent Bless
processes generate SYNs, answer peer SYNs with SYN-ACKs, and complete each
connection with ACKs. The initiator records handshake RTT from SYN creation to
the matching SYN-ACK.

The supplied configuration is intentionally low rate. The PCAP PMD and Linux
veth pair are suitable for functional validation, not line-rate benchmarking.

## What the experiment proves

- both endpoints have working RX and TX queues;
- SYN, SYN-ACK, and ACK packets traverse the test link in both directions;
- the handshake table reaches `ESTABLISHED` rather than timing out every flow;
- `/observe`, `/api/stats`, and `/metrics` publish live RX, TX, handshake, and
  latency data;
- two DPDK processes can run independently on one host.

It does not measure NIC or DUT forwarding performance. Insert a DUT between the
two endpoint interfaces only after this baseline passes.

## Topology

```text
                   Linux host

  +--------------------+                     +--------------------+
  | Bless left         |                     | Bless right        |
  | handshake          |                     | handshake          |
  | lcores 0-1         |                     | lcores 2-3         |
  | file-prefix: left  |                     | file-prefix: right |
  | Observe :8000      |                     | Observe :8001      |
  +---------+----------+                     +----------+---------+
            | net_pcap0                                  | net_pcap0
            | RX/TX                                      | RX/TX
      bless-left0 <========== Linux veth ==========> bless-right0
         10.0.0.1/24                              192.168.1.1/24
```

The IP addresses assigned by Linux are only interface-administration aids.
Bless constructs Ethernet/IPv4/TCP frames directly from the YAML recipes and
does not depend on the kernel TCP stack.

## Configuration design

The endpoint recipes are mirror images:

| Setting | Left | Right |
|---|---|---|
| Config | `conf/config-hs-left.yaml` | `conf/config-hs-right.yaml` |
| Interface | `bless-left0` | `bless-right0` |
| Source IP | `10.0.0.1+1024` | `192.168.1.1` |
| Destination IP | `192.168.1.1` | `10.0.0.1+1024` |
| TCP source port | `10000+100` | `80` |
| TCP destination port | `80` | `10000+100` |
| Source MAC | `02:00:00:00:00:01` | `02:00:00:00:00:02` |
| Destination MAC | `02:00:00:00:00:02` | `02:00:00:00:00:01` |
| Main/worker lcores | `0` / `1` | `2` / `3` |
| DPDK file prefix | `bless-hs-left` | `bless-hs-right` |
| Observe | `127.0.0.1:8000` | `127.0.0.1:8001` |

`hs-mix-ratio: 1000` makes every generated packet part of the stateful
handshake workload. `hs-rate: 64` limits each endpoint worker to approximately
64 initiated connections per second; `batch: 64` controls burst capacity and a
1 ms batch delay prevents a hot busy loop. `latency-hist-enable: true` records
SYN-to-SYN-ACK RTT on whichever endpoint initiated the connection.

Both endpoints initiate connections as well as respond. This deliberately
exercises both directions; the counters on each side need not be identical at
every snapshot. Explicit, mirrored MAC addresses also let the handshake worker
discard any locally injected frame that the PCAP backend captures again, so a
single endpoint cannot produce a false successful handshake by answering
itself.

## Prerequisites

- a Bless build containing the DPDK PCAP PMD;
- Linux `ip` command and permission to create a veth pair;
- at least four CPUs available to this process namespace;
- hugepages configured if required by the local DPDK build.

Check the binary and CPU availability:

```bash
build/release-static/bin/bless version -v
nproc
```

If the host exposes fewer than four CPUs, change the right-side `main-lcore`
and `l` values and run the endpoints in separate CPU namespaces or on separate
hosts. A DPDK main lcore cannot also be that process's packet worker.

## Create the link

Run as root, or through a privilege mechanism that grants `CAP_NET_ADMIN`:

```bash
ip link add bless-left0 type veth peer name bless-right0
ip addr add 10.0.0.1/24 dev bless-left0
ip addr add 192.168.1.1/24 dev bless-right0
ip link set bless-left0 up
ip link set bless-right0 up
```

Verify it before starting Bless:

```bash
ip -br link show bless-left0 bless-right0
```

If those names already exist, inspect them and reuse them only when they are the
intended peer pair. Do not delete an unknown interface merely to run this test.

## Start both endpoints

Start the right endpoint first so it is ready to answer the first SYNs:

```bash
build/release-static/bin/bless conf/config-hs-right.yaml
```

In a second terminal:

```bash
build/release-static/bin/bless conf/config-hs-left.yaml
```

The configurations use different CPU sets, HTTP ports, and DPDK file prefixes.
Do not override them with the same values on a single host.

## View Observe

Open either or both dashboards:

- left: <http://127.0.0.1:8000/observe>
- right: <http://127.0.0.1:8001/observe>

Expected signals after a few seconds:

- RX and TX rates are non-zero;
- `Latency (µs)` reports a non-zero sample count;
- handshake counters show received SYNs/SYN-ACKs and established connections;
- timeout growth is small relative to established growth.

The latency value is application-level handshake RTT: time from constructing a
SYN until Bless receives its matching SYN-ACK. It includes Bless scheduling,
PCAP, veth, peer processing, and the return path. It is not a hardware timestamp
or one-way latency measurement.

The dashboard uses WebSocket updates. For a scriptable check, query the same
snapshot as JSON:

```bash
curl -s http://127.0.0.1:8000/api/stats
curl -s http://127.0.0.1:8001/api/stats
```

Prometheus counters provide a concise pass/fail check:

```bash
curl -s http://127.0.0.1:8000/metrics | \
  grep -E '^bless_(hs_(cps|success_rate|established_total)|lat_samples)'
curl -s http://127.0.0.1:8001/metrics | \
  grep -E '^bless_(hs_(cps|success_rate|established_total)|lat_samples)'
```

A functional pass requires non-zero `bless_hs_established_total` and
`bless_lat_samples` on at least the initiating side. Do not use TX activity
alone as the pass criterion: SYNs can be transmitted while the return path is
broken.

## Troubleshooting

### Observe says RX is idle

Check that both processes are running and each configuration names the correct
veth endpoint:

```bash
ip -s link show bless-left0
ip -s link show bless-right0
```

Also check the startup log for a non-zero RX queue. `net_pcap0,iface=...` is a
TX-only declaration; handshake mode requires the explicit
`rx_iface=...,tx_iface=...` form used by these configurations.

### RX is active but latency has no samples

Inspect the handshake counters. Non-zero `syn_recv` with zero `synack_recv`
means frames arrive but responses do not match an initiated 5-tuple. Confirm
that the left and right IP/port recipes remain exact mirrors and that an
intermediate DUT preserves the tuple.

### One process fails during DPDK initialization

Confirm the two files retain distinct `file-prefix` values and do not share
lcores. Remove only stale DPDK runtime files that you have verified belong to a
previous crashed run; do not remove another active process's namespace.

### High timeout count or unstable rates

PCAP can drop packets under load. Keep this baseline at the supplied rate,
reduce `hs-rate` if necessary, and move to physical DPDK ports for performance
testing. Increasing `hs-timeout-us` can mask slow scheduling but cannot repair a
missing return path.

## Insert a DUT after the baseline

Replace the direct veth peer with two distinct DUT-facing interfaces:

```text
Bless left RX/TX <-> DUT port A <-> DUT forwarding path <-> DUT port B <-> Bless right RX/TX
```

Keep the mirrored address/port recipes, unique process resources, and Observe
ports. First verify L2 reachability and established handshakes at the supplied
rate; only then raise `hs-rate`, queues, or worker count.

## Stop and clean up

Stop both Bless processes with `Ctrl-C`. If the veth pair was created solely for
this experiment, removing either endpoint removes the pair:

```bash
ip link delete bless-left0
```

This deletion is optional and should only be performed after confirming the
interface belongs to this experiment.
