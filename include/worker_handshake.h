#ifndef __WORKER_HANDSHAKE_H__
#define __WORKER_HANDSHAKE_H__

/**
 * @file worker_handshake.h
 * @brief Lightweight TCP handshake engine for connection-stateful testing.
 *
 * Implements a per-worker SYN → SYN-ACK → ACK state machine backed
 * by an open-addressing hash table (HS_HT_SIZE buckets), with
 * chunked timeout cleanup and RST generation.
 *
 * Designed for high-CPS scenarios where the handshake itself is the
 * entropy source: random 5-tuples are generated each burst, and
 * completed connections exercise the DUT's conntrack / session table.
 */

#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_byteorder.h>

#include "worker.h"   /* struct worker, struct handshake_ctx, HS_* constants */

/**
 * Fill a pre-sized mbuf with Eth/IPv4/TCP headers for a handshake packet.
 *
 * The mbuf must already have total_pkt_size =
 * sizeof(Eth) + sizeof(IP) + sizeof(TCP) = 54 appended.
 * HW checksum offload is enabled (IP + TCP).
 *
 * @param m          Pre-allocated mbuf with 54 bytes appended.
 * @param src_mac    Source MAC address.
 * @param dst_mac    Destination MAC address.
 * @param src_ip_be  Source IPv4 address (network byte order).
 * @param dst_ip_be  Destination IPv4 address (network byte order).
 * @param src_port   TCP source port (network byte order).
 * @param dst_port   TCP destination port (network byte order).
 * @param seq_be     TCP sequence number (network byte order).
 * @param ack_be     TCP acknowledgement number (network byte order).
 * @param flags      TCP flags (RTE_TCP_SYN_FLAG, RTE_TCP_ACK_FLAG, etc.).
 */
static inline void
tcp_hs_fill(struct rte_mbuf *m,
	    const struct rte_ether_addr *src_mac,
	    const struct rte_ether_addr *dst_mac,
	    uint32_t src_ip_be, uint32_t dst_ip_be,
	    uint16_t src_port, uint16_t dst_port,
	    uint32_t seq_be, uint32_t ack_be,
	    uint8_t flags)
{
	struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
	rte_ether_addr_copy(src_mac, &eth->src_addr);
	rte_ether_addr_copy(dst_mac, &eth->dst_addr);
	eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

	struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
	memset(ip, 0, sizeof(*ip));
	ip->version_ihl = 0x45;
	ip->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr)
					    + sizeof(struct rte_tcp_hdr));
	ip->time_to_live = 64;
	ip->next_proto_id = IPPROTO_TCP;
	ip->src_addr = src_ip_be;
	ip->dst_addr = dst_ip_be;
	ip->hdr_checksum = 0;
	m->ol_flags |= RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM;

	struct rte_tcp_hdr *tcp = (struct rte_tcp_hdr *)(ip + 1);
	memset(tcp, 0, sizeof(*tcp));
	tcp->src_port = src_port;
	tcp->dst_port = dst_port;
	tcp->sent_seq = seq_be;
	tcp->recv_ack = ack_be;
	tcp->data_off = (sizeof(struct rte_tcp_hdr) / 4) << 4;
	tcp->tcp_flags = flags;
	tcp->rx_win = rte_cpu_to_be_16(65535);
	tcp->cksum = 0;
	m->ol_flags |= RTE_MBUF_F_TX_TCP_CKSUM;
	m->l2_len = sizeof(struct rte_ether_hdr);
	m->l3_len = sizeof(struct rte_ipv4_hdr);
}

/** TCP packet construction helpers (handshake mode). */
#define TCP_PKT_SIZE  (sizeof(struct rte_ether_hdr) \
		       + sizeof(struct rte_ipv4_hdr) \
		       + sizeof(struct rte_tcp_hdr))

/**
 * Lightweight TCP handshake worker mode.
 *
 * Each burst: allocates a mix of SYN (handshake) and stateless TCP
 * traffic according to hs_mix_ratio, sends them, receives responses,
 * generates SYN-ACK / ACK replies, and cleans up timed-out entries
 * with RST packets.
 *
 * Rate-limited by CPS token bucket.  Flow entropy samples are recorded
 * on ESTABLISHED and TIMEOUT events.
 *
 * Requires a device with RX capability (no PCAP iface=lo TX-only).
 *
 * @param w  Per-lcore worker context.
 * @return   0 on clean exit.
 */
int worker_func_handshake(struct worker *w);

#endif /* __WORKER_HANDSHAKE_H__ */
