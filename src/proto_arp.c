#include "bless.h"
#include "bless_plugin.h"

/**
 * @file proto_arp.c
 * @brief ARP packet constructor.
 *
 * Alternates between ARP Request (broadcast) and ARP Reply (unicast)
 * using a per-burst toggle.  Source / target IPs are drawn from the
 * configured Cnode ranges.
 */

uint64_t bless_mbufs_arp(struct rte_mbuf **mbufs, unsigned int n, void *data)
{
	uint64_t tx_bytes = 0;
	const uint16_t total_pkt_size = sizeof(struct rte_ether_hdr) +
		sizeof(struct rte_arp_hdr);
	Cnode *cnode = (Cnode*)data;

	for (int i = 0; i < (int)n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);
		// Set mbuf total size
		assert(m->data_len == 0);
		assert(m->pkt_len == 0);
		if (!rte_pktmbuf_append(m, total_pkt_size)) {
			return 0;
		}
		tx_bytes += total_pkt_size;

		struct rte_ether_hdr *eth_hdr =
			rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
		struct rte_arp_hdr *arp_hdr = (struct rte_arp_hdr *)(eth_hdr + 1);

		static const struct rte_ether_addr dst_mac = {
			{ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }
		};
		static uint64_t flag = 1;
		if (flag++ & 1) {
			rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.src,
					&eth_hdr->src_addr);
			rte_ether_addr_copy(&dst_mac, &eth_hdr->dst_addr);
			eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP);

			arp_hdr->arp_hardware = rte_cpu_to_be_16(RTE_ARP_HRD_ETHER);
			arp_hdr->arp_protocol = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
			arp_hdr->arp_hlen = RTE_ETHER_ADDR_LEN;
			arp_hdr->arp_plen = sizeof(uint32_t);
			arp_hdr->arp_opcode = rte_cpu_to_be_16(RTE_ARP_OP_REQUEST);
			rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.src,
					&arp_hdr->arp_data.arp_sha);
			arp_hdr->arp_data.arp_sip = RANDOM_IP_SRC(cnode);
			rte_ether_addr_copy(&dst_mac, &arp_hdr->arp_data.arp_tha);
			arp_hdr->arp_data.arp_tip = RANDOM_IP_DST(cnode);
		} else {
			rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.src,
					&eth_hdr->src_addr);
			rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.dst,
					&eth_hdr->dst_addr);
			eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP);

			arp_hdr->arp_hardware = rte_cpu_to_be_16(RTE_ARP_HRD_ETHER);
			arp_hdr->arp_protocol = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
			arp_hdr->arp_hlen = RTE_ETHER_ADDR_LEN;
			arp_hdr->arp_plen = sizeof(uint32_t);
			arp_hdr->arp_opcode = rte_cpu_to_be_16(RTE_ARP_OP_REPLY);
			rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.dst,
					&arp_hdr->arp_data.arp_sha);
			arp_hdr->arp_data.arp_sip = RANDOM_IP_DST(cnode);
			rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.src,
					&arp_hdr->arp_data.arp_tha);
			arp_hdr->arp_data.arp_tip = RANDOM_IP_SRC(cnode);
		}
	}

	return tx_bytes;
}


static const struct bless_pkt_type proto_arp = {
	.name = "arp",
	.ether_type = RTE_ETHER_TYPE_IPV4,
	.ip_proto = 0,
	.type_idx = TYPE_ARP,
	.construct = bless_mbufs_arp,
};

static void __attribute__((constructor)) reg_arp(void) {
    bless_register_pkt_type(&proto_arp);
}
