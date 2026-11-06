#include "injector.h"
#include "injector_plugin.h"

/**
 * @file proto_icmp.c
 * @brief ICMP Echo packet constructor.
 *
 * Generates alternating Echo Request / Echo Reply packets with
 * configurable identifiers and payload.
 */

uint64_t injector_mbufs_icmp(struct rte_mbuf **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode*)data;
	struct rte_ether_hdr *eth;
	struct rte_ipv4_hdr *ip;
	const char *payload = cnode->ether.type.ipv4.icmp.payload;
	uint16_t payload_len = cnode->ether.type.ipv4.icmp.payload_len;
	uint64_t tx_bytes = 0;

	const uint16_t l2_len = sizeof(struct rte_ether_hdr);
	const uint16_t l3_len = sizeof(struct rte_ipv4_hdr);
	const uint16_t l4_len = sizeof(struct rte_icmp_hdr);
	uint16_t payload_len_fixed = payload_len;
	{
		uint16_t effective_mtu = cnode->ether.mtu;
		if (cnode->vxlan.enable && cnode->vxlan.wire_mtu) {
			uint16_t vxlan_mtu = cnode->vxlan.wire_mtu;
			if (vxlan_mtu > INJECTOR_SIZEOF_VXLAN + l2_len)
				vxlan_mtu -= INJECTOR_SIZEOF_VXLAN + l2_len;
			else
				vxlan_mtu = 0;
			if (vxlan_mtu && (!effective_mtu || vxlan_mtu < effective_mtu))
				effective_mtu = vxlan_mtu;
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
		tx_bytes += total_pkt_size;

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
		ip->next_proto_id = IPPROTO_ICMP;
		ip->src_addr = RANDOM_IP_SRC(cnode);
		ip->dst_addr = RANDOM_IP_DST(cnode);

		ip->hdr_checksum = 0;
		if (OFFLOAD_IPV4(cnode)) {
			m->ol_flags |= RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM;
		} else {
			ip->hdr_checksum = rte_ipv4_cksum(ip);
		}

		uint32_t flag = 1;
		uint16_t seq_base = (uint16_t)(rte_rdtsc() & 0xFFFF);
		struct rte_icmp_hdr *icmp = (struct rte_icmp_hdr *)(ip + 1);
		if ((flag + i) & 1) {
			icmp->icmp_type = RTE_IP_ICMP_ECHO_REQUEST; // Echo Request
			icmp->icmp_seq_nb = rte_cpu_to_be_16(seq_base + i);
		} else {
			icmp->icmp_type = RTE_IP_ICMP_ECHO_REPLY; // Echo Reply
			icmp->icmp_seq_nb = 0;
		}
		icmp->icmp_code = 0;
		icmp->icmp_ident = rte_cpu_to_be_16(
			random_array_elem_uint16_t(cnode->ether.type.ipv4.icmp.ident,
				cnode->ether.type.ipv4.icmp.n_ident, 0));

		if (cnode->ether.copy_payload && payload && payload_len) {
			rte_memcpy((char *)icmp + sizeof(*icmp), payload, payload_len);
		}

		
		/* checksum */
		icmp->icmp_cksum = 0;
		const uint16_t icmp_len = l4_len + payload_len_fixed;
		icmp->icmp_cksum = icmp_calc_cksum(icmp, icmp_len);

		m->l2_len = l2_len;
		m->l3_len = l3_len;
		m->l4_len = l4_len;

		tx_bytes += total_pkt_size;
	}

	return tx_bytes;
}



static const struct injector_pkt_type proto_icmp = {
	.name = "icmp",
	.ether_type = RTE_ETHER_TYPE_IPV4,
	.ip_proto = 1,
	.type_idx = TYPE_ICMP,
	.construct = injector_mbufs_icmp,
};

static void __attribute__((constructor)) reg_icmp(void) {
    injector_register_pkt_type(&proto_icmp);
}
