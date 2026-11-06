# VXLAN inner-ARP FDIR validation

This guide describes the two-process hardware validation setup for steering
VXLAN-encapsulated inner ARP packets to RX queue 0. Bless generates a finite
packet stream. An external verifier derived from the DPDK `l2fwd` sample can
install `rte_flow` rules on the receiving port and report hardware rule
counters and software RX queue counters. The verifier source is not part of
this repository; keep it in a separate local checkout.

This is a hardware/PMD validation, not a software-only proof. In particular,
successful `rte_flow_validate()` means the PMD accepts the requested
combination, while a queried COUNT value and the queue counters establish that
packets were actually matched and delivered. COUNT query support depends on the
NIC and PMD.

## Rule behavior

The verifier installs two ingress rules on each enabled receiving port:

| Priority | Pattern | Actions | Purpose |
| --- | --- | --- | --- |
| 0 | `ETH / IPV4 / UDP(dst=4789) / VXLAN / ETH(type=ARP)` | `COUNT / QUEUE 0` | Steer VXLAN inner-ARP traffic to RXQ 0 |
| 1 | `ETH` | `COUNT / QUEUE 1` | Catch other Ethernet ingress traffic |

The VXLAN rule does not constrain outer IP addresses or VNI. It does constrain
the outer network header to IPv4 and the UDP destination port to the standard
VXLAN port, 4789. The final ETH item is the inner Ethernet header; its EtherType
is matched against ARP (`0x0806`). The fallback rule is lower priority so the
more specific rule gets the first opportunity to match.

The verifier first calls `rte_flow_validate()` for both rules, then creates the
fallback rule and the VXLAN ARP rule. It queries each COUNT action during its
statistics output and also counts packets received by each RX queue. RXQ 0 is
polled by lcore 0; RXQ 1 is polled by the first worker lcore. Queue assignment
is application software configuration, while `QUEUE` is the hardware steering
action.

## Build and run

Prepare the modified verifier in a separate checkout. Its expected behavior is
the rule and counter logic described above. The previously used local variant
was based on the DPDK 23.11 `l2fwd` sample; obtain a compatible verifier before
following the commands below. Build it against the installed DPDK development
package from its own directory:

```sh
make -C /path/to/external/l2fwd
```

On the receiving host, replace the PCI address with the receiving port and
start the verifier before the sender. It requires lcore 0 as the EAL main lcore
and at least one worker lcore:

```sh
sudo /path/to/external/l2fwd/build/l2fwd \
  -l 0-1 --main-lcore 0 \
  -a 0000:af:00.0 \
  -- -p 0x1 -P -T 1 --no-mac-updating
```

`-P` enables promiscuous mode, useful with the synthetic destination MAC in the
sample sender config. Alternatively, edit both outer and inner destination MAC
values in the config to match the receiving port and omit `-P` if appropriate
for the test environment.

On the sending host, use a local sender config outside the tracked repository
(for example, `l2fwd/config-vxlan-arp-fdir.yaml` in the ignored local directory).
Set `dpdk.allow` to the Bless TX port, then run from the repository root:

```sh
sudo build/release-static/bin/bless l2fwd/config-vxlan-arp-fdir.yaml
```

The local sender config uses one TX worker and requests 1,000 packets. It generates inner
ARP frames, encapsulates them in VXLAN over outer IPv4/UDP destination port
4789, and uses VNI 1001. The VNI is intentionally not part of the FDIR match.
The sender and receiver must be connected so the generated frames reach the
receiving port; the PCI addresses in the two commands refer to their respective
hosts/ports and may differ.

## Expected evidence

At verifier startup, both validation messages should appear, followed by
`FDIR rules created`. After the finite sender run, the target COUNT and RXQ 0
software counter should each be 1,000, with no target traffic on RXQ 1. Other
background traffic can increment the fallback rule and RXQ 1:

```text
VXLAN inner ARP  hits=1000 ...
RX queue 0 (lcore 0):                 1000
RX queue 1 (...):                        0
```

Interpret failures in stages:

1. Validation failure indicates an unsupported or invalid pattern/action for
   the selected device/PMD.
2. Rule creation failure means validation alone was insufficient to install
   the rule; retain the PMD error message.
3. `COUNT` query failure (for example, `action not supported`) means the
   hardware/PMD does not expose that query. It does not by itself prove that
   steering failed; use RX queue counters and packet capture as independent
   evidence, and do not claim a hardware hit count.
4. A target count with no matching RXQ 0 traffic, or RXQ 1 target traffic,
   warrants checking queue mapping, drops, port selection, and whether another
   rule or ingress traffic interferes.

This experiment validates the specific IPv4 VXLAN/ARP case described above; it
does not claim matching of outer IPv6, nonstandard UDP ports, or other inner
EtherTypes. The commands require an external verifier with the stated rule and
counter behavior; an unmodified upstream `l2fwd` does not provide it.
