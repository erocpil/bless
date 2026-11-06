#include "bless.h"
#include "bless_plugin.h"
#include <string.h>

/* Forward-declared here to avoid pulling in mutation.h (which contains
 * non-static function definitions that would cause duplicate symbols). */
struct Mutator {
	char name[32];
	mutation_func func;
};

/**
 * @file proto_ipv6.c
 * @brief IPv6 packet constructor (TCP / UDP alternating).
 *
 * Produces Eth + IPv6 + TCP or Eth + IPv6 + UDP packets with
 * configurable src/dst addresses and ports.  L4 protocol alternates
 * per-packet for within-burst protocol entropy.
 *
 * Registered as a bless plugin extension -- YAML config is parsed
 * by the generic bless_cfg framework under bless.ether.type.ipv6.
 */

uint64_t bless_mbufs_ipv6(struct rte_mbuf **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode *)data;
	struct rte_ether_hdr *eth;
	struct rte_ipv6_hdr *ipv6;
	uint64_t tx_bytes = 0;

	const uint16_t l2_len = sizeof(struct rte_ether_hdr);
	const uint16_t l3_len = sizeof(struct rte_ipv6_hdr);

	if (!cnode->ether.type.ipv6.n_src || !cnode->ether.type.ipv6.n_dst) {
		return 0;
	}

	for (int i = 0; i < (int)n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);

		/*
		 * Alternate TCP / UDP for protocol entropy.  When only one
		 * protocol is configured (e.g. only TCP ports are set), fall
		 * through to the other.
		 */
		int use_tcp = (i & 1) ? 1 : 0;
		uint8_t  l4_proto;
		uint16_t l4_len;
		uint16_t src_port, dst_port;

		if (use_tcp && cnode->ether.type.ipv6.tcp.n_src) {
			l4_proto = IPPROTO_TCP;
			l4_len = sizeof(struct rte_tcp_hdr);
			src_port = RANDOM_IPV6_TCP_SRC(cnode);
			dst_port = RANDOM_IPV6_TCP_DST(cnode);
		} else if (cnode->ether.type.ipv6.udp.n_src) {
			l4_proto = IPPROTO_UDP;
			l4_len = sizeof(struct rte_udp_hdr);
			src_port = RANDOM_IPV6_UDP_SRC(cnode);
			dst_port = RANDOM_IPV6_UDP_DST(cnode);
		} else if (cnode->ether.type.ipv6.tcp.n_src) {
			l4_proto = IPPROTO_TCP;
			l4_len = sizeof(struct rte_tcp_hdr);
			src_port = RANDOM_IPV6_TCP_SRC(cnode);
			dst_port = RANDOM_IPV6_TCP_DST(cnode);
		} else {
			return 0; /* no port config at all */
		}

		const uint16_t total_pkt_size = l2_len + l3_len + l4_len;

		if (!rte_pktmbuf_append(m, total_pkt_size)) {
			return 0;
		}
		tx_bytes += total_pkt_size;

		/* Ethernet */
		eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
		rte_ether_addr_copy((struct rte_ether_addr *)cnode->ether.src,
				    &eth->src_addr);
		rte_ether_addr_copy((struct rte_ether_addr *)cnode->ether.dst,
				    &eth->dst_addr);
		eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6);

		/* IPv6 header */
		ipv6 = (struct rte_ipv6_hdr *)(eth + 1);
		ipv6->vtc_flow = rte_cpu_to_be_32(0x60000000);
		ipv6->payload_len = rte_cpu_to_be_16(l4_len);
		ipv6->proto = l4_proto;
		ipv6->hop_limits = 64;
		random_array_elem_ipv6(ipv6->src_addr, cnode->ether.type.ipv6.src,
			cnode->ether.type.ipv6.n_src,
			cnode->ether.type.ipv6.src_range);
		random_array_elem_ipv6(ipv6->dst_addr, cnode->ether.type.ipv6.dst,
			cnode->ether.type.ipv6.n_dst,
			cnode->ether.type.ipv6.dst_range);

		m->l2_len = l2_len;
		m->l3_len = l3_len;
		m->l4_len = l4_len;

		/* L4 header */
		if (l4_proto == IPPROTO_TCP) {
			struct rte_tcp_hdr *tcp = (struct rte_tcp_hdr *)(ipv6 + 1);
			memset(tcp, 0, l4_len);
			tcp->src_port = src_port;
			tcp->dst_port = dst_port;
			tcp->data_off = ((l4_len - 4) / 4) << 4;  /* minimal header -- no options */
			tcp->tcp_flags = RTE_TCP_PSH_FLAG | RTE_TCP_ACK_FLAG;

			if (OFFLOAD_IPV6(cnode)) {
				m->ol_flags |= RTE_MBUF_F_TX_IPV6 | RTE_MBUF_F_TX_TCP_CKSUM;
				tcp->cksum = rte_ipv6_phdr_cksum(ipv6, m->ol_flags);
			}
		} else {
			struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ipv6 + 1);
			memset(udp, 0, l4_len);
			udp->src_port = src_port;
			udp->dst_port = dst_port;
			udp->dgram_len = rte_cpu_to_be_16(l4_len);

			if (OFFLOAD_IPV6(cnode)) {
				m->ol_flags |= RTE_MBUF_F_TX_IPV6 | RTE_MBUF_F_TX_UDP_CKSUM;
				udp->dgram_cksum = rte_ipv6_phdr_cksum(ipv6, m->ol_flags);
			}
		}
	}

	return tx_bytes;
}

/* Plugin registration */
static void __attribute__((constructor)) register_ipv6_pkt(void)
{
	static const struct bless_pkt_type t = {
		.name       = "ipv6",
		.ether_type = RTE_ETHER_TYPE_IPV6,
		.ip_proto   = IPPROTO_TCP,  /* primary; alternates with UDP at construction */
		.type_idx   = BLESS_AUTO_IDX,
		.construct  = bless_mbufs_ipv6,
	};
	bless_register_pkt_type(&t);
}

/* Erroneous mutations */
/** Corrupt the 4-bit version field in the IPv6 header.  Normal value
 *  is 6; we set it to 4, 0, or 0xF to test gateway version checks. */
uint64_t mutation_ipv6_version(void **mbufs, unsigned int n, void *data)
{
	(void)data;
	uint64_t ok = 0;

	for (unsigned int i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		struct rte_ipv6_hdr *ip6;

		ip6 = rte_pktmbuf_mtod_offset(m, struct rte_ipv6_hdr *, m->l2_len);
		uint32_t vtc = rte_be_to_cpu_32(ip6->vtc_flow);
		vtc = (vtc & 0x0FFFFFFF) | (4u << 28);  /* version = 4 */
		ip6->vtc_flow = rte_cpu_to_be_32(vtc);
		ok++;
	}
	return ok;
}

/** Corrupt the 8-bit Traffic Class field (bits 20-27 of vtc_flow). */
uint64_t mutation_ipv6_traffic_class(void **mbufs, unsigned int n, void *data)
{
	(void)data;
	uint64_t ok = 0;

	for (unsigned int i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		struct rte_ipv6_hdr *ip6;

		ip6 = rte_pktmbuf_mtod_offset(m, struct rte_ipv6_hdr *, m->l2_len);
		uint32_t vtc = rte_be_to_cpu_32(ip6->vtc_flow);
		vtc = (vtc & 0xF00FFFFF) | (0xFFu << 20);  /* TC = 0xFF */
		ip6->vtc_flow = rte_cpu_to_be_32(vtc);
		ok++;
	}
	return ok;
}

/** Corrupt the 20-bit Flow Label (bits 0-19 of vtc_flow). */
uint64_t mutation_ipv6_flow_label(void **mbufs, unsigned int n, void *data)
{
	(void)data;
	uint64_t ok = 0;

	for (unsigned int i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		struct rte_ipv6_hdr *ip6;

		ip6 = rte_pktmbuf_mtod_offset(m, struct rte_ipv6_hdr *, m->l2_len);
		uint32_t vtc = rte_be_to_cpu_32(ip6->vtc_flow);
		vtc = (vtc & 0xFFF00000) | (fast_rand_next() & 0xFFFFF);  /* random label */
		ip6->vtc_flow = rte_cpu_to_be_32(vtc);
		ok++;
	}
	return ok;
}

/** Set Hop Limit to 0 to force ICMPv6 Time Exceeded on the first hop. */
uint64_t mutation_ipv6_hop_limit(void **mbufs, unsigned int n, void *data)
{
	(void)data;
	uint64_t ok = 0;

	for (unsigned int i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		struct rte_ipv6_hdr *ip6;

		ip6 = rte_pktmbuf_mtod_offset(m, struct rte_ipv6_hdr *, m->l2_len);
		ip6->hop_limits = 0;
		ok++;
	}
	return ok;
}

struct Mutator ipv6_mutators[] = {
	{ "version",        mutation_ipv6_version },
	{ "traffic_class",  mutation_ipv6_traffic_class },
	{ "flow_label",     mutation_ipv6_flow_label },
	{ "hop_limit",      mutation_ipv6_hop_limit },
};
