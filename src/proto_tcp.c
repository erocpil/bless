#include "bless.h"
#include "bless_plugin.h"

/**
 * @file proto_tcp.c
 * @brief TCP segment constructor.
 *
 * Produces segments with weighted-random flag distribution
 * (60 % PSH+ACK, 15 % SYN, 10 % SYN+ACK, 10 % plain ACK,
 * 5 % FIN+ACK).  Supports IMIX payload sizing and TSC embedding
 * for one-way delay measurement.
 */

uint64_t bless_mbufs_tcp(struct rte_mbuf **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode*)data;
	const char *payload = cnode->ether.type.ipv4.tcp.payload;
	uint16_t payload_len = cnode->ether.type.ipv4.tcp.payload_len;

	const uint16_t l2_len = sizeof(struct rte_ether_hdr);
	const uint16_t l3_len = sizeof(struct rte_ipv4_hdr);
	const uint16_t l4_len = sizeof(struct rte_tcp_hdr);
	uint16_t payload_len_fixed = payload_len;
	{
		uint16_t effective_mtu = cnode->ether.mtu;
		if (cnode->vxlan.enable && cnode->vxlan.wire_mtu) {
			uint16_t vxlan_mtu = cnode->vxlan.wire_mtu;
			if (vxlan_mtu > BLESS_SIZEOF_VXLAN + l2_len) {
				vxlan_mtu -= BLESS_SIZEOF_VXLAN + l2_len;
			} else {
				vxlan_mtu = 0;
			}
			if (vxlan_mtu && (!effective_mtu || vxlan_mtu < effective_mtu)) {
				effective_mtu = vxlan_mtu;
			}
		}
		if (effective_mtu) {
			payload_len_fixed = effective_mtu - l3_len - l4_len;
			payload_len = min(payload_len, payload_len_fixed);
		}
	}
	/* IMIX: override payload_len with a randomly chosen IMIX size */
	if (cnode->ether.n_imix) {
		uint16_t imix_pl = IMIX_PAYLOAD_LEN(cnode, l3_len, l4_len);
		if (imix_pl) {
			payload_len_fixed = imix_pl;
			payload_len = min(payload_len, payload_len_fixed);
		}
	}
	const uint16_t total_pkt_size = l2_len + l3_len + l4_len + payload_len_fixed;

	/* When latency measurement is enabled, ensure at least 8 bytes of payload */
	if (cnode->latency_hist_enable && payload_len_fixed < 8) {
		payload_len_fixed = 8;
	}

	uint64_t tx_bytes = 0;

	for (int i = 0; i < (int)n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);
		// Set mbuf total size
		assert(m->data_len == 0);
		assert(m->pkt_len == 0);
		if (!rte_pktmbuf_append(m, total_pkt_size)) {
			return 0;
		}

		struct rte_ether_hdr *eth_hdr =
			rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.src,
				&eth_hdr->src_addr);
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.dst,
				&eth_hdr->dst_addr);
		eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

		struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth_hdr + 1);
		memset(ip, 0, sizeof(*ip));
		ip->version_ihl = (4u << 4) | (sizeof(struct rte_ipv4_hdr) >> 2);
		ip->type_of_service = 0;
		ip->total_length = rte_cpu_to_be_16(l3_len + l4_len + payload_len_fixed);
		ip->packet_id = rte_cpu_to_be_16(0);
		ip->fragment_offset = rte_cpu_to_be_16(RTE_IPV4_HDR_DF_FLAG);
		ip->time_to_live = 64;
		ip->next_proto_id = IPPROTO_TCP;
		ip->src_addr = RANDOM_IP_SRC(cnode);
		ip->dst_addr = RANDOM_IP_DST(cnode);
		ip->hdr_checksum = 0;
		if (OFFLOAD_IPV4(cnode)) {
			m->ol_flags |= RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM;
		} else {
			ip->hdr_checksum = rte_ipv4_cksum(ip);
		}

		struct rte_tcp_hdr *tcp = (struct rte_tcp_hdr *)(ip + 1);
		memset(tcp, 0, sizeof(*tcp));
		tcp->src_port = RANDOM_TCP_SRC(cnode);
		tcp->dst_port = RANDOM_TCP_DST(cnode);

		/* weighted random TCP flags (flag diversity for entropy) */
		enum { FLG_PSH_ACK, FLG_SYN, FLG_SYN_ACK, FLG_ACK, FLG_FIN_ACK, FLG_N };
		static const uint8_t flag_bits[FLG_N] = {
			RTE_TCP_PSH_FLAG | RTE_TCP_ACK_FLAG,  /* 60% */
			RTE_TCP_SYN_FLAG,                      /* 15% */
			RTE_TCP_SYN_FLAG | RTE_TCP_ACK_FLAG,  /* 10% */
			RTE_TCP_ACK_FLAG,                      /* 10% */
			RTE_TCP_FIN_FLAG | RTE_TCP_ACK_FLAG,  /* 5% */
		};
		static const uint8_t flag_wt[FLG_N] = { 60, 15, 10, 10, 5 };
		uint32_t rf = fast_rand_next() % 100;
		uint8_t cum = 0;
		int fi;
		for (fi = 0; fi < FLG_N; fi++) { cum += flag_wt[fi];
			if (rf < cum) {
				break;
			}
		}
		if (fi >= FLG_N) {
			fi = FLG_N - 1; /* safety: clamp in case weights don't sum to 100 */
		}

		tcp->sent_seq = rte_cpu_to_be_32(fast_rand_next());
		tcp->recv_ack = 0;
		tcp->data_off = (sizeof(struct rte_tcp_hdr) / 4) << 4;
		tcp->tcp_flags = flag_bits[fi];
		tcp->rx_win = rte_cpu_to_be_16(65535);

		if (cnode->ether.copy_payload && payload && payload_len) {
			rte_memcpy((uint8_t *)tcp + l4_len, payload, payload_len);
		}

		/* latency measurement: embed TSC in TCP payload (first 8 bytes) */
		if (cnode->latency_hist_enable && payload_len_fixed >= 8) {
			uint64_t now = rte_rdtsc();
			rte_memcpy((uint8_t *)tcp + l4_len, &now, 8);
		}

		tcp->cksum = 0;
		if (OFFLOAD_TCP(cnode)) {
			m->ol_flags |= RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_TCP_CKSUM;
		} else {
			tcp->cksum = rte_ipv4_udptcp_cksum(ip, (const void *)tcp);
		}

		m->l2_len = l2_len;
		m->l3_len = l3_len;
		m->l4_len = l4_len;

		tx_bytes += total_pkt_size;
	}

	return tx_bytes;
}



static const struct bless_pkt_type proto_tcp = {
	.name = "tcp",
	.ether_type = RTE_ETHER_TYPE_IPV4,
	.ip_proto = 6,
	.type_idx = TYPE_TCP,
	.construct = bless_mbufs_tcp,
};

static void __attribute__((constructor)) reg_tcp(void) {
    bless_register_pkt_type(&proto_tcp);
}
