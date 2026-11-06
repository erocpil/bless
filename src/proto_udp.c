#include "bless.h"
#include "bless_plugin.h"

/**
 * @file proto_udp.c
 * @brief UDP datagram constructor.
 *
 * Simple stateless UDP datagrams with configurable ports and
 * payload.  Used as the baseline protocol for latency measurement
 * (TSC embedded in the first 8 payload bytes).
 */

uint64_t bless_mbufs_udp(struct rte_mbuf **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode*)data;
	struct rte_ether_hdr *eth;
	struct rte_ipv4_hdr *ip;
	struct rte_udp_hdr *udp;
	const char *payload = cnode->ether.type.ipv4.udp.payload;
	uint16_t payload_len = cnode->ether.type.ipv4.udp.payload_len;
	uint64_t tx_bytes = 0;

	const uint16_t l2_len = sizeof(struct rte_ether_hdr);
	const uint16_t l3_len = sizeof(struct rte_ipv4_hdr);
	const uint16_t l4_len = sizeof(struct rte_udp_hdr);
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

	for (int i = 0; i < (int)n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);
		// Reserve mbuf headroom for VXLAN encap
		assert(m->data_len == 0);
		assert(m->pkt_len == 0);
		if (!rte_pktmbuf_append(m, total_pkt_size)) {
			return 0;
		}
		tx_bytes += m->data_len;

		eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.src,
				&eth->src_addr);
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.dst,
				&eth->dst_addr);
		eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

		ip = (struct rte_ipv4_hdr *)(eth + 1);
		ip->version_ihl = 0x45;
		ip->type_of_service = 0;
		ip->total_length = rte_cpu_to_be_16(l3_len + l4_len + payload_len_fixed);
		ip->packet_id = htons((uint16_t)rte_rand());
		ip->fragment_offset = 0;
		ip->time_to_live = 64;
		ip->next_proto_id = IPPROTO_UDP;
		ip->src_addr = RANDOM_IP_SRC(cnode);
		ip->dst_addr = RANDOM_IP_DST(cnode);

		ip->hdr_checksum = 0;
		if (OFFLOAD_IPV4(cnode)) {
			m->ol_flags |= RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM;
		} else {
			ip->hdr_checksum = rte_ipv4_cksum(ip);
		}

		udp = (struct rte_udp_hdr *)(ip + 1);
		udp->src_port = RANDOM_UDP_SRC(cnode);
		udp->dst_port = RANDOM_UDP_DST(cnode);
		udp->dgram_len = htons(l4_len + payload_len_fixed);
		udp->dgram_cksum = 0;
		if (cnode->ether.copy_payload && payload && payload_len) {
			rte_memcpy((uint8_t *)udp + l4_len, payload, payload_len);
		}

		/* latency histogram: embed rte_rdtsc() as first 8 bytes of UDP payload
		 * (overwrites payload preamble -- shift not needed since TSC replaces
		 * the first 8 bytes and the rest of payload follows at offset 8).
		 * Only when latency_hist_enable is set and there's room. */
		if (cnode->latency_hist_enable && payload_len_fixed >= 8) {
			uint64_t now_tsc = rte_rdtsc();
			rte_memcpy((uint8_t *)udp + l4_len, &now_tsc, 8);
		}

		if (OFFLOAD_UDP(cnode)) {
			m->ol_flags |= RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_UDP_CKSUM;
		} else {
			udp->dgram_cksum = rte_ipv4_udptcp_cksum(ip, udp);
		}

		m->l2_len = l2_len;
		m->l3_len = l3_len;
		m->l4_len = l4_len;
	}

	return tx_bytes;
}


static const struct bless_pkt_type proto_udp = {
	.name = "udp",
	.ether_type = RTE_ETHER_TYPE_IPV4,
	.ip_proto = 17,
	.type_idx = TYPE_UDP,
	.construct = bless_mbufs_udp,
};

static void __attribute__((constructor)) reg_udp(void) {
    bless_register_pkt_type(&proto_udp);
}
