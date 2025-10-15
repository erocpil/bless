#ifndef __MUTATION_H__
#define __MUTATION_H__

#include "bless.h"
#include "config.h"

struct Mutator {
	char name[32];
	mutation_func func;
};

struct Mutation {
	char name[32];
	struct Mutator *mutator;
	size_t count;
};

uint64_t mutation_udp_cksum(void **mbufs, unsigned int n, void *data);
uint64_t mutation_mac_vlan(void **mbufs, unsigned int n, void *data)
{
	// return mutation_udp_cksum(mbufs, n, data);

	Cnode *cnode = (Cnode*)data;
	uint64_t tx_bytes = 0, mutated = 0;

	// 需要的报文头空间: Ethernet + VLAN + IPv4 + ICMP
	const uint16_t pkt_size = sizeof(struct rte_ether_hdr) +
		sizeof(struct rte_vlan_hdr) +
		sizeof(struct rte_ipv4_hdr) +
		sizeof(struct rte_icmp_hdr);

	for (unsigned int i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);

		char *pkt_data = rte_pktmbuf_append(m, pkt_size);
		if (!pkt_data) {
			printf("rte_pktmbuf_append(vlan)\n");
			continue;
		}

		// 填充 Ethernet + VLAN
		struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)pkt_data;
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.src, &eth_hdr->src_addr);
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.dst, &eth_hdr->dst_addr);
		eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_VLAN);

		struct rte_vlan_hdr *vlan_hdr = (struct rte_vlan_hdr *)(eth_hdr + 1);
		vlan_hdr->vlan_tci = rte_cpu_to_be_16(rte_rdtsc() & 0xFFF);
		vlan_hdr->eth_proto = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

		// 填充 IPv4
		struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(vlan_hdr + 1);
		memset(ip_hdr, 0, sizeof(*ip_hdr));
		ip_hdr->version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);
		ip_hdr->type_of_service = 0;
		ip_hdr->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) +
				sizeof(struct rte_icmp_hdr));
		ip_hdr->packet_id = rte_cpu_to_be_16(1);
		ip_hdr->fragment_offset = 0;
		ip_hdr->time_to_live = 225;
		ip_hdr->next_proto_id = IPPROTO_ICMP;
		ip_hdr->src_addr = RANDOM_IP_SRC(cnode);
		ip_hdr->dst_addr = RANDOM_IP_DST(cnode);
		ip_hdr->hdr_checksum = 0;
		ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);

		m->l2_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_vlan_hdr);
		m->l3_len = sizeof(struct rte_ipv4_hdr);
		m->ol_flags = RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_UDP_CKSUM;

#if 1
		// 填充 ICMP (Echo Request)
		struct rte_icmp_hdr *icmp_hdr = (struct rte_icmp_hdr *)(ip_hdr + 1);
		memset(icmp_hdr, 0, sizeof(*icmp_hdr));
		icmp_hdr->icmp_type = RTE_IP_ICMP_ECHO_REPLY;
		icmp_hdr->icmp_code = 0;
		/* FIXME */
		icmp_hdr->icmp_ident = rte_cpu_to_be_16(rte_rdtsc() & (unsigned short int)-1);
		icmp_hdr->icmp_seq_nb = rte_cpu_to_be_16(1);
		icmp_hdr->icmp_cksum = 0;
		icmp_hdr->icmp_cksum = icmp_calc_cksum(icmp_hdr, sizeof(struct rte_icmp_hdr));
#endif

		mutated++;
		tx_bytes += pkt_size;
	}

	return tx_bytes;
	return mutated;
}

/* ip_be 为网络字节序（big-endian） */
static void ipv4_to_mcast_mac(uint32_t ip_be, struct rte_ether_addr *mac)
{
	/* 转成 host 序便于按位操作 */
	uint32_t ip = rte_be_to_cpu_32(ip_be);
	/* 23 bits */
	uint32_t low23 = ip & 0x7FFFFF;
	uint8_t b0 = 0x01, b1 = 0x00, b2 = 0x5e;
	/* 7 bits -> 保证最高位为 0 */
	uint8_t b3 = (low23 >> 16) & 0x7F;
	uint8_t b4 = (low23 >> 8) & 0xFF;
	uint8_t b5 = low23 & 0xFF;

	mac->addr_bytes[0] = b0;
	mac->addr_bytes[1] = b1;
	mac->addr_bytes[2] = b2;
	mac->addr_bytes[3] = b3;
	mac->addr_bytes[4] = b4;
	mac->addr_bytes[5] = b5;
}

uint64_t mutation_mac_multicast(void **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode*)data;
	uint64_t tx_bytes = 0, mutated = 0;
	struct rte_ether_hdr *eth;
	struct rte_ipv4_hdr *ip4;
	struct rte_udp_hdr *udp;
	const char *payload = cnode->ether.type.ip.udp.payload;
	uint16_t payload_len = cnode->ether.type.ip.udp.payload_len;
	uint16_t eth_hdr_len = sizeof(struct rte_ether_hdr);
	uint16_t ip_hdr_len  = sizeof(struct rte_ipv4_hdr);
	uint16_t udp_hdr_len = sizeof(struct rte_udp_hdr);
	uint16_t total_len = eth_hdr_len + ip_hdr_len + udp_hdr_len + payload_len;

	for (unsigned int i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);

		/* 确保有足够空间，追加 total_len 字节 */
		char *pkt_data = rte_pktmbuf_append(m, total_len);
		if (!pkt_data) {
			continue;
		}

		/* 指向各层头部 */
		eth = (struct rte_ether_hdr *)pkt_data;
		ip4 = (struct rte_ipv4_hdr *)(pkt_data + eth_hdr_len);
		udp = (struct rte_udp_hdr *)((uint8_t *)ip4 + ip_hdr_len);
		uint8_t *udppayload = (uint8_t *)udp + udp_hdr_len;

		/* 以太网头: 目的 MAC = 组播 MAC, 源 MAC 可以取端口的 MAC 或自定义 */
		struct rte_ether_addr dst_mac;
		uint32_t dst_ip_be = 0;
		uint64_t tsc = rte_rdtsc();
		tsc = tsc ^ (tsc >> 8);
		int mcast_type = tsc % 3;
		switch (mcast_type) {
			case 0:
				/* 本地子网控制（例如 OSPF, RIP2, IGMP） */
				// dst_ip_be = rte_cpu_to_be_32((224 << 24) | 1);
				dst_ip_be = rte_cpu_to_be_32((224 << 24) | 1);
				break;
			case 1:
				/* 全球可路由组播 */
				dst_ip_be = rte_cpu_to_be_32((238 << 24) | 1);
				break;
			case 2:
				/* 管理域组播（组织内部使用） */
				dst_ip_be = rte_cpu_to_be_32((239 << 24) | 1);
			default:
				break;
		}
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.src, &dst_mac);
		ipv4_to_mcast_mac(dst_ip_be, &dst_mac);

		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.src, &eth->src_addr);
		rte_ether_addr_copy(&dst_mac, &eth->dst_addr);
		eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

		/* IPv4 头 */
		ip4->version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);
		ip4->type_of_service = 0; /* DSCP/ECN 可在此设置 */
		ip4->total_length = rte_cpu_to_be_16(ip_hdr_len + udp_hdr_len + payload_len);
		ip4->packet_id = rte_cpu_to_be_16(0);
		ip4->fragment_offset = rte_cpu_to_be_16(0);
		ip4->time_to_live = mcast_type ? 225 : 1;
		ip4->next_proto_id = IPPROTO_UDP;
		ip4->src_addr = RANDOM_IP_SRC(cnode);
		ip4->dst_addr = dst_ip_be;
		ip4->hdr_checksum = 0;
		/* 计算 ipv4 头校验和 */
		ip4->hdr_checksum = rte_ipv4_cksum(ip4);

		/* UDP 头 */
		udp->src_port = RANDOM_UDP_SRC(cnode);
		udp->dst_port = RANDOM_UDP_DST(cnode);
		udp->dgram_len = rte_cpu_to_be_16(udp_hdr_len + payload_len);
		udp->dgram_cksum = 0; /* 可启用 offload 或计算伪首部校验和 */

		/* 复制 payload */
		if (payload_len && payload) {
			rte_memcpy(udppayload, payload, payload_len);
		}

		/* 设置 mbuf 长度字段，注意 l2/l3/l4 长度 */
		m->pkt_len = total_len;
		m->data_len = total_len;
		/* l2/l3/l4 len 字段（有些 DPDK 版本使用 l2_len/l3_len/l4_len） */
		m->l2_len = eth_hdr_len;
		m->l3_len = ip_hdr_len;
		m->l4_len = udp_hdr_len;

		/* 如果网卡支持 offload，可以打开 L3/L4 校验 offload（可选）：
		 * m->ol_flags |= PKT_TX_IPV4 | PKT_TX_UDP_CKSUM;
		 * 然后把 udp->dgram_cksum 设置为 rte_cpu_to_be_16(0) 并在 tx 时让 NIC 计算。
		 */
	}

	return tx_bytes;
	return mutated;
}

static void ipv6_multicast_mac(const struct in6_addr *ipv6, struct rte_ether_addr *mac)
{
	// 前两字节固定 33:33
	mac->addr_bytes[0] = 0x33;
	mac->addr_bytes[1] = 0x33;

	// 取 IPv6 地址最后 4 字节
	const uint8_t *ip_bytes = (const uint8_t *)ipv6;
	mac->addr_bytes[2] = ip_bytes[12];
	mac->addr_bytes[3] = ip_bytes[13];
	mac->addr_bytes[4] = ip_bytes[14];
	mac->addr_bytes[5] = ip_bytes[15];
}

/** mutation_mac_ipv6() - multicast ipv6 packet
*/
uint64_t mutation_mac_ipv6(void **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode*)data;
	uint64_t tx_bytes = 0, mutated = 0;
	const char *ipv6_src_addr = "2001:db8::1";
	/* IPv6 组播地址 */
	const char *ipv6_dst_addr = "ff02::1";
	const char *payload = "ipv6 udp payload.";
	const uint16_t payload_len = strlen(payload);
	const uint16_t udp_len = sizeof(struct rte_udp_hdr) + payload_len;
	const uint16_t ipv6_len = sizeof(struct rte_ipv6_hdr) + udp_len;
	const uint16_t pkt_len = sizeof(struct rte_ether_hdr) + ipv6_len;

	for (unsigned int i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);

		m->data_len = pkt_len;
		m->pkt_len  = pkt_len;

		uint8_t *pkt_data = rte_pktmbuf_mtod(m, uint8_t *);

		/* ---------------- Ethernet Header ---------------- */
		struct rte_ether_hdr *eth = (struct rte_ether_hdr *)pkt_data;
		struct rte_ether_addr dst_mac;

		struct in6_addr src_addr, dst_addr;
		inet_pton(AF_INET6, ipv6_src_addr, &src_addr);
		inet_pton(AF_INET6, ipv6_dst_addr, &dst_addr);

		// 生成组播MAC
		ipv6_multicast_mac(&dst_addr, &dst_mac);

		rte_ether_addr_copy(&dst_mac, &eth->dst_addr);
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.src, &eth->src_addr);
		eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6);

		/* ---------------- IPv6 Header ---------------- */
		struct rte_ipv6_hdr *ip6 = (struct rte_ipv6_hdr *)(eth + 1);
		// Version = 6
		ip6->vtc_flow = rte_cpu_to_be_32(6 << 28);
		ip6->payload_len = rte_cpu_to_be_16(udp_len);
		ip6->proto = IPPROTO_UDP;
		// 本地链路组播通常为 1
		ip6->hop_limits = 1;

		rte_memcpy(&ip6->src_addr, &src_addr, sizeof(struct in6_addr));
		rte_memcpy(&ip6->dst_addr, &dst_addr, sizeof(struct in6_addr));

		/* ---------------- UDP Header ---------------- */
		struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip6 + 1);
		udp->src_port = rte_cpu_to_be_16(1234);
		udp->dst_port = rte_cpu_to_be_16(4321);
		udp->dgram_len = rte_cpu_to_be_16(udp_len);
		udp->dgram_cksum = 0;

		/* ---------------- Payload ---------------- */
		if (payload && payload_len) {
			rte_memcpy((char *)(udp + 1), payload, payload_len);
		}

		/* ---------------- UDP Checksum ---------------- */
		udp->dgram_cksum = rte_ipv6_udptcp_cksum(ip6, udp);
	}

	return tx_bytes;
	return mutated;
}

uint64_t mutation_arp_src(void **mbufs, unsigned int n, void *data)
{
	const uint16_t total_pkt_size = sizeof(struct rte_ether_hdr) + sizeof(struct rte_arp_hdr);
	Cnode *cnode = (Cnode*)data;
	uint64_t tx_bytes = 0, mutated = 0;

	for (int i = 0; i < (int)n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);
		// 设置 mbuf 大小
		assert(m->data_len == 0);
		assert(m->pkt_len == 0);
		if (!rte_pktmbuf_append(m, total_pkt_size)) {
			return 0;
		}
		tx_bytes += total_pkt_size;

		// 填充 Ethernet Header
		struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
		struct rte_arp_hdr *arp_hdr = (struct rte_arp_hdr *)(eth_hdr + 1);

		// 构造以太网头部
		static const struct rte_ether_addr dst_mac = {
			{ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }
		};
		static const struct rte_ether_addr dst_mac0 = {
			{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }
		};
		static uint64_t flag = 1;
		if (flag++ & 1) {
			rte_ether_addr_copy((struct rte_ether_addr*)&cnode->ether.src, &eth_hdr->src_addr);
			rte_ether_addr_copy(&dst_mac, &eth_hdr->dst_addr);
			eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP);

			// 构造 ARP 请求头部
			arp_hdr->arp_hardware = rte_cpu_to_be_16(RTE_ARP_HRD_ETHER);
			arp_hdr->arp_protocol = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
			arp_hdr->arp_hlen = RTE_ETHER_ADDR_LEN;
			arp_hdr->arp_plen = sizeof(uint32_t);
			arp_hdr->arp_opcode = rte_cpu_to_be_16(RTE_ARP_OP_REQUEST);
			rte_ether_addr_copy((struct rte_ether_addr*)&cnode->ether.src, &arp_hdr->arp_data.arp_sha);
			arp_hdr->arp_data.arp_sip = RANDOM_IP_SRC(cnode);
			rte_ether_addr_copy(&dst_mac0, &arp_hdr->arp_data.arp_tha);
			arp_hdr->arp_data.arp_tip = RANDOM_IP_DST(cnode);
		} else {
			/* yes, src for src, and dst for dst */
			rte_ether_addr_copy((struct rte_ether_addr*)&cnode->ether.src, &eth_hdr->src_addr);
			rte_ether_addr_copy((struct rte_ether_addr*)&cnode->ether.dst, &eth_hdr->dst_addr);
			eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP);

			arp_hdr->arp_hardware = rte_cpu_to_be_16(RTE_ARP_HRD_ETHER);
			arp_hdr->arp_protocol = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
			arp_hdr->arp_hlen = RTE_ETHER_ADDR_LEN;
			arp_hdr->arp_plen = sizeof(uint32_t);
			arp_hdr->arp_opcode = rte_cpu_to_be_16(RTE_ARP_OP_REPLY);
			rte_ether_addr_copy((struct rte_ether_addr*)&cnode->ether.dst, &arp_hdr->arp_data.arp_sha);
			arp_hdr->arp_data.arp_sip = RANDOM_IP_DST(cnode);
			rte_ether_addr_copy((struct rte_ether_addr*)&cnode->ether.src, &arp_hdr->arp_data.arp_tha);
			arp_hdr->arp_data.arp_tip = RANDOM_IP_SRC(cnode);
		}
	}

	return tx_bytes;
	return mutated;
}

uint64_t mutation_ip_version(void **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode*)data;
	uint64_t tx_bytes = 0, mutated = 0;

	// 需要的报文头空间: Ethernet + IPv4 + ICMP
	const uint16_t pkt_size = sizeof(struct rte_ether_hdr) +
		sizeof(struct rte_ipv4_hdr) +
		sizeof(struct rte_icmp_hdr);

	for (unsigned int i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);

		char *pkt_data = rte_pktmbuf_append(m, pkt_size);
		if (!pkt_data)
			continue;

		// 填充 Ethernet
		struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)pkt_data;
		/* use inner's mac address */
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.src, &eth_hdr->src_addr);
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.dst, &eth_hdr->dst_addr);
		eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

		// 填充 IPv4
		struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
		memset(ip_hdr, 0, sizeof(*ip_hdr));
		/* mutation */
		ip_hdr->version_ihl   = (5 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);
		ip_hdr->type_of_service = 0;
		ip_hdr->total_length  = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) +
				sizeof(struct rte_icmp_hdr));
		ip_hdr->packet_id     = rte_cpu_to_be_16(1);
		ip_hdr->fragment_offset = 0;
		ip_hdr->time_to_live  = 225;
		ip_hdr->next_proto_id = IPPROTO_ICMP;
		// FIXME
		ip_hdr->src_addr = RANDOM_IP_SRC(cnode);
		ip_hdr->dst_addr = RANDOM_IP_DST(cnode);
		ip_hdr->hdr_checksum  = 0;
		ip_hdr->hdr_checksum  = rte_ipv4_cksum(ip_hdr);

		// 填充 ICMP (Echo Request)
		struct rte_icmp_hdr *icmp_hdr = (struct rte_icmp_hdr *)(ip_hdr + 1);
		memset(icmp_hdr, 0, sizeof(*icmp_hdr));
		icmp_hdr->icmp_type = RTE_IP_ICMP_ECHO_REQUEST;
		icmp_hdr->icmp_code = 0;
		icmp_hdr->icmp_ident = rte_cpu_to_be_16(rte_rdtsc() & (unsigned short int)-1);
		icmp_hdr->icmp_seq_nb = rte_cpu_to_be_16(1);
		icmp_hdr->icmp_cksum = 0;
		icmp_hdr->icmp_cksum = icmp_calc_cksum(icmp_hdr, sizeof(struct rte_icmp_hdr));

		mutated++;
		tx_bytes += pkt_size;
	}

	return tx_bytes;
	return mutated;
}

uint64_t mutation_ip_ihl(void **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode*)data;
	uint64_t tx_bytes = 0, mutated = 0;

	// 需要的报文头空间: Ethernet + IPv4 + ICMP
	const uint16_t pkt_size = sizeof(struct rte_ether_hdr) +
		sizeof(struct rte_ipv4_hdr) +
		sizeof(struct rte_icmp_hdr);

	for (unsigned int i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);

		char *pkt_data = rte_pktmbuf_append(m, pkt_size);
		if (!pkt_data)
			continue;

		// 填充 Ethernet
		struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)pkt_data;
		/* use inner's mac address */
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.src, &eth_hdr->src_addr);
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.dst, &eth_hdr->dst_addr);
		eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

		// 填充 IPv4
		struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
		memset(ip_hdr, 0, sizeof(*ip_hdr));
		ip_hdr->version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4 * 2);
		ip_hdr->type_of_service = 0;
		ip_hdr->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) +
				sizeof(struct rte_icmp_hdr));
		ip_hdr->packet_id = rte_cpu_to_be_16(1);
		ip_hdr->fragment_offset = 0;
		ip_hdr->time_to_live = 225;
		ip_hdr->next_proto_id = IPPROTO_ICMP;
		// FIXME
		ip_hdr->src_addr = RANDOM_IP_SRC(cnode);
		ip_hdr->dst_addr = RANDOM_IP_DST(cnode);
		ip_hdr->hdr_checksum = 0;
		ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);

		// 填充 ICMP (Echo Request)
		struct rte_icmp_hdr *icmp_hdr = (struct rte_icmp_hdr *)(ip_hdr + 1);
		memset(icmp_hdr, 0, sizeof(*icmp_hdr));
		icmp_hdr->icmp_type = RTE_IP_ICMP_ECHO_REQUEST;
		icmp_hdr->icmp_code = 0;
		icmp_hdr->icmp_ident = rte_cpu_to_be_16(rte_rdtsc() & (unsigned short int)-1);
		icmp_hdr->icmp_seq_nb = rte_cpu_to_be_16(1);
		icmp_hdr->icmp_cksum = 0;
		icmp_hdr->icmp_cksum = icmp_calc_cksum(icmp_hdr, sizeof(struct rte_icmp_hdr));

		mutated++;
		tx_bytes += pkt_size;
	}

	return tx_bytes;
	return mutated;
}

/*
 * Differentiated Services (DiffServ) (RFC 2474, RFC 2597, RFC 3246)
 | 类别                      | 二进制        | 十进制   | 说明         |
 | ------------------------- | ------------- | ----- | ---------- |
 | Default (Best Effort)     | 000000        | 0     | 默认，无QoS保障  |
 | EF (Expedited Forwarding) | 101110        | 46    | 高优先级，低延迟   |
 | AF11–AF43                 | 多组          | 10–38 | 分级保障类      |
 | CS0–CS7 (Class Selector)  | 000000–111000 | 0–56  | 兼容旧 ToS 设计 |
 */
uint64_t mutation_ip_dscp(void **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode*)data;
	uint64_t tx_bytes = 0, mutated = 0;

	// 需要的报文头空间: Ethernet + IPv4 + ICMP
	const uint16_t pkt_size = sizeof(struct rte_ether_hdr) +
		sizeof(struct rte_ipv4_hdr) +
		sizeof(struct rte_icmp_hdr);

	for (unsigned int i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);

		char *pkt_data = rte_pktmbuf_append(m, pkt_size);
		if (!pkt_data)
			continue;

		// 填充 Ethernet
		struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)pkt_data;
		/* use inner's mac address */
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.src, &eth_hdr->src_addr);
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.dst, &eth_hdr->dst_addr);
		eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

		// 填充 IPv4
		struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
		memset(ip_hdr, 0, sizeof(*ip_hdr));
		ip_hdr->version_ihl   = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);
		/* mutation */
		ip_hdr->type_of_service = 57 << 2;
		ip_hdr->total_length  = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) +
				sizeof(struct rte_icmp_hdr));
		ip_hdr->packet_id     = rte_cpu_to_be_16(1);
		ip_hdr->fragment_offset = 0;
		ip_hdr->time_to_live  = 225;
		ip_hdr->next_proto_id = IPPROTO_ICMP;
		// FIXME
		ip_hdr->src_addr = RANDOM_IP_SRC(cnode);
		ip_hdr->dst_addr = RANDOM_IP_DST(cnode);
		ip_hdr->hdr_checksum  = 0;
		ip_hdr->hdr_checksum  = rte_ipv4_cksum(ip_hdr);

		// 填充 ICMP (Echo Request)
		struct rte_icmp_hdr *icmp_hdr = (struct rte_icmp_hdr *)(ip_hdr + 1);
		memset(icmp_hdr, 0, sizeof(*icmp_hdr));
		icmp_hdr->icmp_type = RTE_IP_ICMP_ECHO_REQUEST;
		icmp_hdr->icmp_code = 0;
		icmp_hdr->icmp_ident = rte_cpu_to_be_16(rte_rdtsc() & (unsigned short int)-1);
		icmp_hdr->icmp_seq_nb = rte_cpu_to_be_16(1);
		icmp_hdr->icmp_cksum = 0;
		icmp_hdr->icmp_cksum = icmp_calc_cksum(icmp_hdr, sizeof(struct rte_icmp_hdr));

		mutated++;
		tx_bytes += pkt_size;
	}

	return tx_bytes;
	return mutated;
}

uint64_t mutation_ip_ecn(void **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode*)data;
	uint64_t tx_bytes = 0, mutated = 0;

	// 需要的报文头空间: Ethernet + IPv4 + ICMP
	const uint16_t pkt_size = sizeof(struct rte_ether_hdr) +
		sizeof(struct rte_ipv4_hdr) +
		sizeof(struct rte_icmp_hdr);

	for (unsigned int i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);

		char *pkt_data = rte_pktmbuf_append(m, pkt_size);
		if (!pkt_data)
			continue;

		// 填充 Ethernet
		struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)pkt_data;
		/* use inner's mac address */
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.src, &eth_hdr->src_addr);
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.dst, &eth_hdr->dst_addr);
		eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

		// 填充 IPv4
		struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
		memset(ip_hdr, 0, sizeof(*ip_hdr));
		ip_hdr->version_ihl   = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);
		/* mutation: ecn of 0, 1, 2, 3 */
		ip_hdr->type_of_service = rte_rdtsc() & 3;
		ip_hdr->total_length  = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) +
				sizeof(struct rte_icmp_hdr));
		ip_hdr->packet_id     = rte_cpu_to_be_16(1);
		ip_hdr->fragment_offset = 0;
		ip_hdr->time_to_live  = 225;
		ip_hdr->next_proto_id = IPPROTO_ICMP;
		// FIXME
		ip_hdr->src_addr = RANDOM_IP_SRC(cnode);
		ip_hdr->dst_addr = RANDOM_IP_DST(cnode);
		ip_hdr->hdr_checksum  = 0;
		ip_hdr->hdr_checksum  = rte_ipv4_cksum(ip_hdr);

		// 填充 ICMP (Echo Request)
		struct rte_icmp_hdr *icmp_hdr = (struct rte_icmp_hdr *)(ip_hdr + 1);
		memset(icmp_hdr, 0, sizeof(*icmp_hdr));
		icmp_hdr->icmp_type = RTE_IP_ICMP_ECHO_REQUEST;
		icmp_hdr->icmp_code = 0;
		icmp_hdr->icmp_ident = rte_cpu_to_be_16(rte_rdtsc() & (unsigned short int)-1);
		icmp_hdr->icmp_seq_nb = rte_cpu_to_be_16(1);
		icmp_hdr->icmp_cksum = 0;
		icmp_hdr->icmp_cksum = icmp_calc_cksum(icmp_hdr, sizeof(struct rte_icmp_hdr));

		mutated++;
		tx_bytes += pkt_size;
	}

	return tx_bytes;
	return mutated;
}

uint64_t mutation_ip_total_length(void **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode*)data;
	uint64_t tx_bytes = 0, mutated = 0;
	const char *payload = "mutation ipv4 length.";
	const uint16_t payload_len = strlen(payload) + 1;
	const uint16_t udp_len = sizeof(struct rte_udp_hdr) + payload_len;
	const uint16_t ipv4_len = sizeof(struct rte_ipv4_hdr) + udp_len;
	const uint16_t pkt_len = sizeof(struct rte_ether_hdr) + ipv4_len;

	for (unsigned int i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);

		char *pkt_data = rte_pktmbuf_append(m, pkt_len);
		if (!pkt_data) {
			continue;
		}

		// 填充 Ethernet
		struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)pkt_data;
		/* use inner's mac address */
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.src, &eth_hdr->src_addr);
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.dst, &eth_hdr->dst_addr);
		eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

		// 填充 IPv4
		struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
		memset(ip_hdr, 0, sizeof(*ip_hdr));
		ip_hdr->version_ihl   = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);
		ip_hdr->type_of_service = 0;
		/* mutation */
		ip_hdr->total_length = rte_cpu_to_be_16(ipv4_len);
		ip_hdr->packet_id     = rte_cpu_to_be_16(1);
		ip_hdr->fragment_offset = 0;
		ip_hdr->time_to_live  = 225;
		ip_hdr->next_proto_id = IPPROTO_UDP;
		// FIXME
		ip_hdr->src_addr = RANDOM_IP_SRC(cnode);
		ip_hdr->dst_addr = RANDOM_IP_DST(cnode);
		ip_hdr->hdr_checksum  = 0;
		ip_hdr->hdr_checksum  = rte_ipv4_cksum(ip_hdr);

		/* ---------------- UDP Header ---------------- */
		struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip_hdr + 1);
		udp->src_port = rte_cpu_to_be_16(10);
		udp->dst_port = rte_cpu_to_be_16(100);
		udp->dgram_len = rte_cpu_to_be_16(udp_len);
		udp->dgram_cksum = 0;

		/* ---------------- Payload ---------------- */
		if (payload && payload_len) {
			char *p = (char *)(udp + 1);
			rte_memcpy(p, payload, payload_len);
		}

		/* ---------------- UDP Checksum ---------------- */
		udp->dgram_cksum = 0; // rte_ipv4_udptcp_cksum(ip_hdr, udp);

#if 0
		rte_pktmbuf_dump(stdout, m, 1000);
		printf("udp len %u\n", udp_len);
		printf("pkt_len %u data_len %u\n\n", m->pkt_len, m->data_len);
#endif

		mutated++;
		tx_bytes += pkt_len;
	}

	return tx_bytes;
	return mutated;
}

uint64_t mutation_ip_id(void **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode*)data;
	uint64_t tx_bytes = 0, mutated = 0;
	const char *payload = "mutation ipv4 id";
	const uint16_t payload_len = strlen(payload) + 1;
	const uint16_t udp_len = sizeof(struct rte_udp_hdr) + payload_len;
	const uint16_t ipv4_len = sizeof(struct rte_ipv4_hdr) + udp_len;
	const uint16_t pkt_len = sizeof(struct rte_ether_hdr) + ipv4_len;

	for (unsigned int i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);

		char *pkt_data = rte_pktmbuf_append(m, pkt_len);
		if (!pkt_data)
			continue;

		// 填充 Ethernet
		struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)pkt_data;
		/* use inner's mac address */
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.src, &eth_hdr->src_addr);
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.dst, &eth_hdr->dst_addr);
		eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

		// 填充 IPv4
		struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
		memset(ip_hdr, 0, sizeof(*ip_hdr));
		ip_hdr->version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);
		ip_hdr->type_of_service = 0;
		ip_hdr->total_length = rte_cpu_to_be_16(ipv4_len);
		ip_hdr->packet_id = rte_cpu_to_be_16(0);
		ip_hdr->fragment_offset = 0;
		ip_hdr->time_to_live = 225;
		ip_hdr->next_proto_id = IPPROTO_UDP;
		// FIXME
		ip_hdr->src_addr = RANDOM_IP_SRC(cnode);
		ip_hdr->dst_addr = RANDOM_IP_DST(cnode);
		ip_hdr->hdr_checksum = 0;
		ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);

		/* ---------------- UDP Header ---------------- */
		struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip_hdr + 1);
		udp->src_port = rte_cpu_to_be_16(54321);
		udp->dst_port = rte_cpu_to_be_16(12346);
		/* mutation */
		udp->dgram_len = rte_cpu_to_be_16(udp_len);
		udp->dgram_cksum = 0;

		/* ---------------- Payload ---------------- */
		if (payload && payload_len) {
			rte_memcpy((char *)(udp + 1), payload, payload_len);
		}

		/* ---------------- UDP Checksum ---------------- */
		udp->dgram_cksum = 0; // rte_ipv4_udptcp_cksum(ip_hdr, udp);

		mutated++;
		tx_bytes += pkt_len;
	}

	return tx_bytes;
	return mutated;
}

uint64_t mutation_ip_flags(void **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode*)data;
	uint64_t tx_bytes = 0, mutated = 0;
	const char *payload = "mutation ipv4 flags.";
	const uint16_t payload_len = strlen(payload);
	const uint16_t udp_len = sizeof(struct rte_udp_hdr) + payload_len;
	const uint16_t ipv4_len = sizeof(struct rte_ipv4_hdr) + udp_len;
	const uint16_t pkt_len = sizeof(struct rte_ether_hdr) + ipv4_len;

	for (unsigned int i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);

		char *pkt_data = rte_pktmbuf_append(m, pkt_len);
		if (!pkt_data) {
			printf("FIXME\n");
			continue;
		}

		// 填充 Ethernet
		struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)pkt_data;
		/* use inner's mac address */
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.src, &eth_hdr->src_addr);
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.dst, &eth_hdr->dst_addr);
		eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

		// 填充 IPv4
		struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
		memset(ip_hdr, 0, sizeof(*ip_hdr));
		ip_hdr->version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);
		ip_hdr->type_of_service = 0;
		ip_hdr->total_length = rte_cpu_to_be_16(ipv4_len);
		ip_hdr->packet_id = rte_cpu_to_be_16(10);
		uint16_t frag_off = 0;
		frag_off |= RTE_IPV4_HDR_MF_FLAG;
		ip_hdr->fragment_offset = rte_cpu_to_be_16(frag_off);
		ip_hdr->time_to_live  = 225;
		ip_hdr->next_proto_id = IPPROTO_UDP;
		// FIXME
		ip_hdr->src_addr = RANDOM_IP_SRC(cnode);
		ip_hdr->dst_addr = RANDOM_IP_DST(cnode);
		ip_hdr->hdr_checksum = 0;
		ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);

		/* ---------------- UDP Header ---------------- */
		struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip_hdr + 1);
		udp->src_port = rte_cpu_to_be_16(12347);
		udp->dst_port = rte_cpu_to_be_16(54321);
		/* mutation */
		udp->dgram_len = rte_cpu_to_be_16(udp_len);
		udp->dgram_cksum = 0;

		/* ---------------- Payload ---------------- */
		if (payload && payload_len) {
			// MF set, wireshark won't show payload
			rte_memcpy((char *)(udp + 1), payload, payload_len);
		}

		/* ---------------- UDP Checksum ---------------- */
		udp->dgram_cksum = 0; // rte_ipv4_udptcp_cksum(ip_hdr, udp);

		mutated++;
		tx_bytes += pkt_len;
	}

	return tx_bytes;
	return mutated;
}

uint64_t mutation_ip_fragment_offset(void **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode*)data;
	uint64_t tx_bytes = 0, mutated = 0;
	const char *payload = "mutation ipv4 fragment offset.";
	const uint16_t payload_len = strlen(payload);
	const uint16_t udp_len = sizeof(struct rte_udp_hdr) + payload_len;
	const uint16_t ipv4_len = sizeof(struct rte_ipv4_hdr) + udp_len;
	const uint16_t pkt_len = sizeof(struct rte_ether_hdr) + ipv4_len;

	for (unsigned int i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);

		char *pkt_data = rte_pktmbuf_append(m, pkt_len);
		if (!pkt_data)
			continue;

		// 填充 Ethernet
		struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)pkt_data;
		/* use inner's mac address */
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.src, &eth_hdr->src_addr);
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.dst, &eth_hdr->dst_addr);
		eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

		// 填充 IPv4
		struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
		memset(ip_hdr, 0, sizeof(*ip_hdr));
		ip_hdr->version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);
		ip_hdr->type_of_service = 0;
		ip_hdr->total_length = rte_cpu_to_be_16(ipv4_len);
		ip_hdr->packet_id = rte_cpu_to_be_16(11);
		uint16_t frag_off = 1 << 1;
		ip_hdr->fragment_offset = rte_cpu_to_be_16(frag_off);
		ip_hdr->time_to_live = 225;
		ip_hdr->next_proto_id = IPPROTO_UDP;
		// FIXME
		ip_hdr->src_addr = RANDOM_IP_SRC(cnode);
		ip_hdr->dst_addr = RANDOM_IP_DST(cnode);
		ip_hdr->hdr_checksum  = 0;
		ip_hdr->hdr_checksum  = rte_ipv4_cksum(ip_hdr);

		/* ---------------- UDP Header ---------------- */
		struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip_hdr + 1);
		udp->src_port = rte_cpu_to_be_16(12348);
		udp->dst_port = rte_cpu_to_be_16(54321);
		/* mutation */
		udp->dgram_len = rte_cpu_to_be_16(udp_len);
		udp->dgram_cksum = 0;

		/* ---------------- Payload ---------------- */
		if (payload && payload_len) {
			rte_memcpy((char *)(udp + 1), payload, payload_len);
		}

		/* ---------------- UDP Checksum ---------------- */
		udp->dgram_cksum = 0; // rte_ipv4_udptcp_cksum(ip_hdr, udp);

		mutated++;
		tx_bytes += pkt_len;
	}

	return tx_bytes;
	return mutated;
}

uint64_t mutation_ip_ttl(void **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode*)data;
	uint64_t tx_bytes = 0, mutated = 0;
	const char *payload = "mutation ipv4 ttl.";
	const uint16_t payload_len = strlen(payload) + 1;
	const uint16_t udp_len = sizeof(struct rte_udp_hdr) + payload_len;
	const uint16_t ipv4_len = sizeof(struct rte_ipv4_hdr) + udp_len;
	const uint16_t pkt_len = sizeof(struct rte_ether_hdr) + ipv4_len;

	for (unsigned int i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);

		char *pkt_data = rte_pktmbuf_append(m, pkt_len);
		if (!pkt_data) {
			continue;
		}

		// 填充 Ethernet
		struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)pkt_data;
		/* use inner's mac address */
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.src, &eth_hdr->src_addr);
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.dst, &eth_hdr->dst_addr);
		eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

		// 填充 IPv4
		struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
		memset(ip_hdr, 0, sizeof(*ip_hdr));
		ip_hdr->version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);
		ip_hdr->type_of_service = 0;
		ip_hdr->total_length = rte_cpu_to_be_16(ipv4_len);
		ip_hdr->packet_id = rte_cpu_to_be_16(1);
		ip_hdr->fragment_offset = 0;
		/* mutation */
		ip_hdr->time_to_live = 225; // (uint8_t)-1;
		// ip_hdr->time_to_live = 0;
		ip_hdr->next_proto_id = IPPROTO_UDP;
		ip_hdr->src_addr = RANDOM_IP_SRC(cnode);
		ip_hdr->dst_addr = RANDOM_IP_DST(cnode);
		ip_hdr->hdr_checksum = 0;
		ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);

		/* ---------------- UDP Header ---------------- */
		struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip_hdr + 1);
		udp->src_port = rte_cpu_to_be_16(54321);
		udp->dst_port = rte_cpu_to_be_16(12349);
		/* mutation */
		udp->dgram_len = rte_cpu_to_be_16(udp_len);
		udp->dgram_cksum = 0;

		/* ---------------- Payload ---------------- */
		if (payload && payload_len) {
			rte_memcpy((char*)(udp + 1), payload, payload_len);
		}

		/* ---------------- UDP Checksum ---------------- */
		udp->dgram_cksum = 0; // rte_ipv4_udptcp_cksum(ip_hdr, udp);

		m->l2_len = sizeof(struct rte_ether_hdr);
		m->l3_len = sizeof(struct rte_ipv4_hdr);
		m->l4_len = sizeof(struct rte_udp_hdr);
		assert(m->pkt_len == pkt_len);
		assert(m->data_len == pkt_len);
		mutated++;
		tx_bytes += pkt_len;
	}

	return tx_bytes;
	return mutated;
}

uint64_t mutation_ip_protocol(void **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode*)data;
	uint64_t tx_bytes = 0, mutated = 0;
	const char *payload = "mutation ipv4 protocol.";
	const uint16_t payload_len = strlen(payload) + 1;
	const uint16_t udp_len = sizeof(struct rte_udp_hdr) + payload_len;
	const uint16_t ipv4_len = sizeof(struct rte_ipv4_hdr) + udp_len;
	const uint16_t pkt_len = sizeof(struct rte_ether_hdr) + ipv4_len;

	for (unsigned int i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);

		char *pkt_data = rte_pktmbuf_append(m, pkt_len);
		if (!pkt_data)
			continue;

		// 填充 Ethernet
		struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)pkt_data;
		/* use inner's mac address */
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.src, &eth_hdr->src_addr);
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.dst, &eth_hdr->dst_addr);
		eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

		// 填充 IPv4
		struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
		memset(ip_hdr, 0, sizeof(*ip_hdr));
		ip_hdr->version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);
		ip_hdr->type_of_service = 0;
		ip_hdr->total_length = rte_cpu_to_be_16(ipv4_len);
		ip_hdr->packet_id = rte_cpu_to_be_16(1);
		ip_hdr->fragment_offset = 0;
		ip_hdr->time_to_live = 225; // 64;
		ip_hdr->next_proto_id = 0;
		// FIXME
		ip_hdr->src_addr = RANDOM_IP_SRC(cnode);
		ip_hdr->dst_addr = RANDOM_IP_DST(cnode);
		ip_hdr->hdr_checksum = 0;
		ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);

		/* ---------------- UDP Header ---------------- */
		struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip_hdr + 1);
		udp->src_port = rte_cpu_to_be_16(54321);
		udp->dst_port = rte_cpu_to_be_16(12350);
		/* mutation */
		udp->dgram_len = rte_cpu_to_be_16(udp_len);
		udp->dgram_cksum = 0;

		/* ---------------- Payload ---------------- */
		if (payload && payload_len) {
			rte_memcpy((char *)(udp + 1), payload, payload_len);
		}

		/* ---------------- UDP Checksum ---------------- */
		udp->dgram_cksum = 0; // rte_ipv4_udptcp_cksum(ip_hdr, udp);

		mutated++;
		tx_bytes += pkt_len;
	}

	return tx_bytes;
	return mutated;
}

uint64_t mutation_ip_cksum(void **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode*)data;
	uint64_t tx_bytes = 0, mutated = 0;

	// 需要的报文头空间: Ethernet + IPv4 + ICMP
	const uint16_t pkt_size = sizeof(struct rte_ether_hdr) +
		sizeof(struct rte_ipv4_hdr) +
		sizeof(struct rte_icmp_hdr);

	for (unsigned int i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);

		char *pkt_data = rte_pktmbuf_append(m, pkt_size);
		if (!pkt_data) {
			continue;
		}

		// 填充 Ethernet
		struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)pkt_data;
		/* use inner's mac address */
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.src, &eth_hdr->src_addr);
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.dst, &eth_hdr->dst_addr);
		eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

		// 填充 IPv4
		struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
		memset(ip_hdr, 0, sizeof(*ip_hdr));
		ip_hdr->version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);
		ip_hdr->type_of_service = 0;
		ip_hdr->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) +
				sizeof(struct rte_icmp_hdr));
		ip_hdr->packet_id = rte_cpu_to_be_16(1);
		ip_hdr->fragment_offset = 0;
		ip_hdr->time_to_live = 225; // 64;
		ip_hdr->next_proto_id = IPPROTO_ICMP;
		// FIXME
		ip_hdr->src_addr = RANDOM_IP_SRC(cnode);
		ip_hdr->dst_addr = RANDOM_IP_DST(cnode);
		ip_hdr->hdr_checksum = 0;
		/* mutation */
		/* FIXME */
		ip_hdr->hdr_checksum = rte_cpu_to_be_16(1); // rte_ipv4_cksum(ip_hdr);

		// 填充 ICMP (Echo Request)
		struct rte_icmp_hdr *icmp_hdr = (struct rte_icmp_hdr *)(ip_hdr + 1);
		memset(icmp_hdr, 0, sizeof(*icmp_hdr));
		icmp_hdr->icmp_type = RTE_IP_ICMP_ECHO_REQUEST;
		icmp_hdr->icmp_code = 0;
		icmp_hdr->icmp_ident = rte_cpu_to_be_16(rte_rdtsc() & (unsigned short int)-1);
		icmp_hdr->icmp_seq_nb = rte_cpu_to_be_16(1);
		icmp_hdr->icmp_cksum = 0;
		icmp_hdr->icmp_cksum = icmp_calc_cksum(icmp_hdr, sizeof(struct rte_icmp_hdr));

		mutated++;
		tx_bytes += pkt_size;
	}

	return tx_bytes;
	return mutated;
}

/**
 * build an ipsec packet
 */

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/aes.h>

#define AES_KEY_LEN 16   // AES-128-CBC
#define AES_IV_LEN 16
#define HMAC_KEY_LEN 32
#define ICV_TRUNC_LEN 16 // 取 HMAC-SHA256 前16字节作为示例 ICV

/* 一些示例常量（实际请从 SA 获得） */
static const uint32_t ESP_SPI = 0xdeadbeef;
static uint32_t esp_seq = 1; /* 应由 SA 维护并单调递增 */

static uint8_t aes_key[AES_KEY_LEN] = {
	0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
	0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f
};
static uint8_t hmac_key[HMAC_KEY_LEN] = {
	0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
	0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,
	0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,
	0x28,0x29,0x2a,0x2b,0x2c,0x2d,0x2e,0x2f
};

/* 辅助：PKCS#7 填充 */
static int pkcs7_pad(const uint8_t *in, int inlen, uint8_t **out, int block_len, int *outlen) {
	int pad = block_len - (inlen % block_len);
	*outlen = inlen + pad;
	*out = malloc(*outlen);
	if (!*out) return -1;
	memcpy(*out, in, inlen);
	memset(*out + inlen, pad, pad);
	return 0;
}

/* 用 AES-CBC 加密 (OpenSSL EVP)，返回 ciphertext，caller 负责 free。 */
static int aes_cbc_encrypt(const uint8_t *plaintext, int pt_len,
		const uint8_t *key, const uint8_t *iv,
		uint8_t **ciphertext, int *ct_len)
{
	EVP_CIPHER_CTX *ctx = NULL;
	int len, ciphertext_len = 0;
	int rc = -1;

	ctx = EVP_CIPHER_CTX_new();
	if (!ctx) goto out;

	if (!EVP_EncryptInit_ex(ctx, EVP_aes_128_cbc(), NULL, key, iv)) goto out;

	*ciphertext = malloc(pt_len + AES_BLOCK_SIZE);
	if (!*ciphertext) goto out;

	if (!EVP_EncryptUpdate(ctx, *ciphertext, &len, plaintext, pt_len)) goto out;
	ciphertext_len = len;
	if (!EVP_EncryptFinal_ex(ctx, *ciphertext + len, &len)) goto out;
	ciphertext_len += len;

	*ct_len = ciphertext_len;
	rc = 0;
out:
	if (ctx) EVP_CIPHER_CTX_free(ctx);
	if (rc != 0 && *ciphertext) { free(*ciphertext); *ciphertext = NULL; }
	return rc;
}

/* 计算 HMAC-SHA256，返回 digest 长度（digest buf 已分配） */
static int calc_hmac_sha256(const uint8_t *key, int keylen,
		const uint8_t *data, int datalen,
		uint8_t *out, unsigned int *outlen)
{
	unsigned char *res = HMAC(EVP_sha256(), key, keylen, data, datalen, out, outlen);
	if (!res) return -1;
	return 0;
}

/* 构造内层 (inner) IPv4 + UDP 报文作为 ESP 明文 */
static int build_inner_ipv4_udp(uint8_t **out, int *outlen) {
	/* 简单构造： inner IPv4 + UDP + payload */
	const char payload[] = "DPDK IPSEC inner payload";
	int payload_len = sizeof(payload)-1;
	int udp_len = sizeof(struct rte_udp_hdr) + payload_len;
	int ip_len = sizeof(struct rte_ipv4_hdr) + udp_len;
	uint8_t *buf = malloc(ip_len);
	if (!buf) return -1;

	struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)buf;
	struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(buf + sizeof(struct rte_ipv4_hdr));
	uint8_t *data = (uint8_t *)(udp + 1);

	memset(buf, 0, ip_len);

	ip->version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr)/4);
	ip->type_of_service = 0;
	ip->total_length = rte_cpu_to_be_16(ip_len);
	ip->packet_id = rte_cpu_to_be_16(0x1234);
	ip->fragment_offset = rte_cpu_to_be_16(0);
	ip->time_to_live = 225; // 64;
	ip->next_proto_id = IPPROTO_UDP;
	ip->src_addr = rte_cpu_to_be_32((192<<24)|(168<<16)|(0<<8)|10); /* 192.168.0.10 */
	ip->dst_addr = rte_cpu_to_be_32((192<<24)|(168<<16)|(0<<8)|20); /* 192.168.0.20 */
	ip->hdr_checksum = 0;
	ip->hdr_checksum = rte_ipv4_cksum(ip);

	udp->src_port = rte_cpu_to_be_16(50000);
	udp->dst_port = rte_cpu_to_be_16(60000);
	udp->dgram_len = rte_cpu_to_be_16(udp_len);
	udp->dgram_cksum = 0; /* 不计算内层 UDP 校验和 */

	memcpy(data, payload, payload_len);

	*out = buf;
	*outlen = ip_len;
	return 0;
}

/* 主函数：构造并返回一个填充好 ESP 的 mbuf（caller 负责发送或释放） */
uint64_t build_ipv4_esp_tunnel(struct rte_mbuf *m, void *data)
{
	Cnode *cnode = (Cnode*)data;
	uint8_t *inner = NULL;
	int inner_len = 0;
	if (build_inner_ipv4_udp(&inner, &inner_len) != 0) {
		printf("build inner failed\n");
		return 0;
	}

	/* PKCS7 填充（AES-CBC 需要按块填充） */
	uint8_t *padded = NULL;
	int padded_len = 0;
	if (pkcs7_pad(inner, inner_len, &padded, AES_BLOCK_SIZE, &padded_len) != 0) {
		free(inner);
		return 0;
	}
	free(inner);

	/* 生成随机 IV */
	uint8_t iv[AES_IV_LEN];
	if (RAND_bytes(iv, AES_IV_LEN) != 1) {
		printf("RAND_bytes failed\n");
		free(padded);
		return 0;
	}

	/* AES-CBC 加密 */
	uint8_t *ciphertext = NULL;
	int ct_len = 0;
	if (aes_cbc_encrypt(padded, padded_len, aes_key, iv, &ciphertext, &ct_len) != 0) {
		printf("encrypt failed\n");
		free(padded);
		return 0;
	}
	free(padded);

	/* 构造 ESP header (SPI(4 bytes) | Seq(4 bytes)) -- 网络字节序 */
	uint32_t spi_be = rte_cpu_to_be_32(ESP_SPI);
	uint32_t seq_be = rte_cpu_to_be_32(esp_seq++);

	/* 计算 ICV = HMAC-SHA256( ESP header || IV || ciphertext )，取前 ICV_TRUNC_LEN */
	int auth_input_len = 4 + 4 + AES_IV_LEN + ct_len;
	uint8_t *auth_input = malloc(auth_input_len);
	if (!auth_input) { free(ciphertext); return 0; }
	uint8_t *p = auth_input;
	memcpy(p, &spi_be, 4); p += 4;
	memcpy(p, &seq_be, 4); p += 4;
	memcpy(p, iv, AES_IV_LEN); p += AES_IV_LEN;
	memcpy(p, ciphertext, ct_len);

	uint8_t hmac_full[EVP_MAX_MD_SIZE];
	unsigned int hmac_len = 0;
	if (calc_hmac_sha256(hmac_key, HMAC_KEY_LEN, auth_input, auth_input_len, hmac_full, &hmac_len) != 0) {
		printf("hmac failed\n");
		free(auth_input); free(ciphertext);
		return 0;
	}
	free(auth_input);

	/* ICV 取前 ICV_TRUNC_LEN bytes */
	uint8_t icv[ICV_TRUNC_LEN];
	memcpy(icv, hmac_full, ICV_TRUNC_LEN);

	/* 外层 IPv4 + ESP 数据总长度计算 */
	int esp_payload_len = 4 + 4 /* esp header */ + AES_IV_LEN + ct_len + ICV_TRUNC_LEN;
	int outer_ip_hdr_len = sizeof(struct rte_ipv4_hdr);
	int eth_hdr_len = sizeof(struct rte_ether_hdr);
	int total_pkt_len = eth_hdr_len + outer_ip_hdr_len + esp_payload_len;

	char *pkt = rte_pktmbuf_append(m, total_pkt_len);
	if (!pkt) {
		printf("rte_pktmbuf_append failed\n");
		rte_pktmbuf_free(m);
		free(ciphertext);
		return 0;
	}

	/* ---------------- 以太头 (示例固定 MAC) ---------------- */
	struct rte_ether_hdr *eth = (struct rte_ether_hdr *)pkt;
	rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.src, &eth->src_addr);
	rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.dst, &eth->dst_addr);
	eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

	/* ---------------- 外层 IPv4 头 ---------------- */
	struct rte_ipv4_hdr *oip = (struct rte_ipv4_hdr *)(pkt + eth_hdr_len);
	memset(oip, 0, sizeof(*oip));
	oip->version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr)/4);
	oip->type_of_service = 0;
	oip->total_length = rte_cpu_to_be_16(outer_ip_hdr_len + esp_payload_len);
	oip->packet_id = rte_cpu_to_be_16(0x9999);
	oip->fragment_offset = rte_cpu_to_be_16(0);
	oip->time_to_live = 225; // 64;
	oip->next_proto_id = IPPROTO_ESP; /* 50 */
	oip->src_addr = RANDOM_IP_SRC(cnode);
	oip->dst_addr = RANDOM_IP_DST(cnode);
	oip->hdr_checksum = 0;
	oip->hdr_checksum = rte_ipv4_cksum(oip);

	/* ---------------- 填充 ESP 部分 ---------------- */
	uint8_t *esp_ptr = (uint8_t *)(oip + 1);
	p = esp_ptr;
	memcpy(p, &spi_be, 4); p += 4;
	memcpy(p, &seq_be, 4); p += 4;
	memcpy(p, iv, AES_IV_LEN); p += AES_IV_LEN;
	memcpy(p, ciphertext, ct_len); p += ct_len;
	memcpy(p, icv, ICV_TRUNC_LEN); p += ICV_TRUNC_LEN;

	/* 设置 mbuf 长度字段与 l2/l3 长度（便于后续可能的 offload） */
	m->pkt_len = total_pkt_len;
	m->data_len = total_pkt_len;
	m->l2_len = eth_hdr_len;
	m->l3_len = outer_ip_hdr_len;

	free(ciphertext);

	return total_pkt_len;
}

uint64_t mutation_ip_ipsec(void **mbufs, unsigned int n, void *data)
{
	uint64_t tx_bytes = 0, mutated = 0;
	for (unsigned int i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);
		build_ipv4_esp_tunnel(m, data);
	}

	return tx_bytes;
	return mutated;
}

uint64_t mutation_icmp_type(void **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode*)data;
	uint64_t tx_bytes = 0, mutated = 0;

	// 需要的报文头空间: Ethernet + IPv4 + ICMP
	const uint16_t pkt_size = sizeof(struct rte_ether_hdr) +
		sizeof(struct rte_ipv4_hdr) +
		sizeof(struct rte_icmp_hdr);

	for (unsigned int i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);

		char *pkt_data = rte_pktmbuf_append(m, pkt_size);
		if (!pkt_data) {
			continue;
		}

		// 填充 Ethernet
		struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)pkt_data;
		/* use inner's mac address */
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.src, &eth_hdr->src_addr);
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.dst, &eth_hdr->dst_addr);
		eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

		// 填充 IPv4
		struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
		memset(ip_hdr, 0, sizeof(*ip_hdr));
		/* mutation */
		ip_hdr->version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);
		ip_hdr->type_of_service = 0;
		ip_hdr->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) +
				sizeof(struct rte_icmp_hdr));
		ip_hdr->packet_id = rte_cpu_to_be_16(1);
		ip_hdr->fragment_offset = 0;
		ip_hdr->time_to_live = 225; // 64;
		ip_hdr->next_proto_id = IPPROTO_ICMP;
		// FIXME
		ip_hdr->src_addr = RANDOM_IP_SRC(cnode);
		ip_hdr->dst_addr = RANDOM_IP_DST(cnode);
		ip_hdr->hdr_checksum = 0;
		ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);

		// 填充 ICMP (Echo Request)
		struct rte_icmp_hdr *icmp_hdr = (struct rte_icmp_hdr *)(ip_hdr + 1);
		memset(icmp_hdr, 0, sizeof(*icmp_hdr));
		icmp_hdr->icmp_type = 15 + ((rte_rdtsc() >> 2) & 3);
		icmp_hdr->icmp_code = 0;
		icmp_hdr->icmp_ident = rte_cpu_to_be_16(rte_rdtsc() & (unsigned short int)-1);
		icmp_hdr->icmp_seq_nb = rte_cpu_to_be_16(1);
		icmp_hdr->icmp_cksum = 0;
		icmp_hdr->icmp_cksum = icmp_calc_cksum(icmp_hdr, sizeof(struct rte_icmp_hdr));

		mutated++;
		tx_bytes += pkt_size;
	}

	return tx_bytes;
	return mutated;
}

/** mutation_tcp_syn_flood() - build tcp syn
*/
uint64_t mutation_tcp_syn_flood(void **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode*)data;
	uint64_t tx_bytes = 0, mutated = 0;

	// L2 + L3 + L4 头部空间
	uint16_t pkt_size = sizeof(struct rte_ether_hdr) +
		sizeof(struct rte_ipv4_hdr) +
		sizeof(struct rte_tcp_hdr);

	for (unsigned int i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);

		char *pkt_data = rte_pktmbuf_append(m, pkt_size);
		if (!pkt_data) {
			continue;
		}

		// Ethernet header
		struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)pkt_data;
		struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
		struct rte_tcp_hdr *tcp_hdr = (struct rte_tcp_hdr *)(ip_hdr + 1);

		// 设置 MAC 地址（示例）
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.src, &eth_hdr->src_addr);
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.dst, &eth_hdr->dst_addr);
		eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

		// IPv4 header
		ip_hdr->version_ihl = RTE_IPV4_VHL_DEF;
		ip_hdr->type_of_service = 0;
		ip_hdr->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_tcp_hdr));
		ip_hdr->packet_id = rte_cpu_to_be_16(1);
		ip_hdr->fragment_offset = 0;
		ip_hdr->time_to_live = 225; // 64;
		ip_hdr->next_proto_id = IPPROTO_TCP;
		ip_hdr->src_addr = RANDOM_IP_SRC(cnode);
		ip_hdr->dst_addr = RANDOM_IP_DST(cnode);
		ip_hdr->hdr_checksum = 0;
		ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);

		// TCP header
		tcp_hdr->src_port = RANDOM_TCP_SRC(cnode);
		tcp_hdr->dst_port = RANDOM_TCP_DST(cnode);
		tcp_hdr->sent_seq = rte_cpu_to_be_32(1001);
		tcp_hdr->recv_ack = 0;
		tcp_hdr->data_off = (sizeof(struct rte_tcp_hdr) / 4) << 4;  // 头部长度 (20B)
		tcp_hdr->tcp_flags = RTE_TCP_SYN_FLAG;
		tcp_hdr->rx_win = rte_cpu_to_be_16(65535);
		tcp_hdr->cksum = 0;
		tcp_hdr->tcp_urp = 0;

		// 计算 TCP 校验和
		tcp_hdr->cksum = rte_ipv4_udptcp_cksum(ip_hdr, tcp_hdr);
	}

	return tx_bytes;
	return mutated;
}

uint64_t mutation_tcp_data_off(void **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode*)data;
	uint64_t tx_bytes = 0, mutated = 0;

	// L2 + L3 + L4 头部空间
	uint16_t pkt_size = sizeof(struct rte_ether_hdr) +
		sizeof(struct rte_ipv4_hdr) +
		sizeof(struct rte_tcp_hdr);

	for (unsigned int i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);

		char *pkt_data = rte_pktmbuf_append(m, pkt_size);
		if (!pkt_data) {
			continue;
		}

		// Ethernet header
		struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)pkt_data;
		struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
		struct rte_tcp_hdr *tcp_hdr = (struct rte_tcp_hdr *)(ip_hdr + 1);

		// 设置 MAC 地址（示例）
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.src, &eth_hdr->src_addr);
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.dst, &eth_hdr->dst_addr);
		eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

		// IPv4 header
		ip_hdr->version_ihl = RTE_IPV4_VHL_DEF;
		ip_hdr->type_of_service = 0;
		ip_hdr->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_tcp_hdr));
		ip_hdr->packet_id = rte_cpu_to_be_16(1);
		ip_hdr->fragment_offset = 0;
		ip_hdr->time_to_live = 225; // 64;
		ip_hdr->next_proto_id = IPPROTO_TCP;
		ip_hdr->src_addr = RANDOM_IP_SRC(cnode);
		ip_hdr->dst_addr = RANDOM_IP_DST(cnode);
		ip_hdr->hdr_checksum = 0;
		ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);

		// TCP header
		tcp_hdr->src_port = RANDOM_TCP_SRC(cnode);
		tcp_hdr->dst_port = RANDOM_TCP_DST(cnode);
		tcp_hdr->sent_seq = rte_cpu_to_be_32(rte_rdtsc() & (uint32_t)-1);
		tcp_hdr->recv_ack = 1;
		/* mutation */
		tcp_hdr->data_off = (uint8_t)-1; // ((sizeof(struct rte_tcp_hdr) / 4) << 4) + (rte_rdtsc() & 0xf);
		tcp_hdr->tcp_flags = RTE_TCP_ACK_FLAG;
		tcp_hdr->rx_win = rte_cpu_to_be_16(65535);
		tcp_hdr->cksum = 0;
		tcp_hdr->tcp_urp = 0;

		// 计算 TCP 校验和
		tcp_hdr->cksum = rte_ipv4_udptcp_cksum(ip_hdr, tcp_hdr);
	}

	return tx_bytes;
	return mutated;
}

uint64_t mutation_tcp_flags(void **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode*)data;
	uint64_t tx_bytes = 0, mutated = 0;

	// L2 + L3 + L4 头部空间
	uint16_t pkt_size = sizeof(struct rte_ether_hdr) +
		sizeof(struct rte_ipv4_hdr) +
		sizeof(struct rte_tcp_hdr);

	for (unsigned int i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);

		char *pkt_data = rte_pktmbuf_append(m, pkt_size);
		if (!pkt_data) {
			continue;
		}

		// Ethernet header
		struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)pkt_data;
		struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
		struct rte_tcp_hdr *tcp_hdr = (struct rte_tcp_hdr *)(ip_hdr + 1);

		// 设置 MAC 地址（示例）
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.src, &eth_hdr->src_addr);
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.dst, &eth_hdr->dst_addr);
		eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

		// IPv4 header
		ip_hdr->version_ihl = RTE_IPV4_VHL_DEF;
		ip_hdr->type_of_service = 0;
		ip_hdr->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_tcp_hdr));
		ip_hdr->packet_id = rte_cpu_to_be_16(1);
		ip_hdr->fragment_offset = 0;
		ip_hdr->time_to_live = 225; // 64;
		ip_hdr->next_proto_id = IPPROTO_TCP;
		ip_hdr->src_addr = RANDOM_IP_SRC(cnode);
		ip_hdr->dst_addr = RANDOM_IP_DST(cnode);
		ip_hdr->hdr_checksum = 0;
		ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);

		// TCP header
		tcp_hdr->src_port = RANDOM_TCP_SRC(cnode);
		tcp_hdr->dst_port = RANDOM_TCP_DST(cnode);
		tcp_hdr->sent_seq = rte_cpu_to_be_32(rte_rdtsc() & (uint32_t)-1);
		tcp_hdr->recv_ack = 1;
		tcp_hdr->data_off = ((sizeof(struct rte_tcp_hdr) / 4) << 4);
		/* mutation */
		tcp_hdr->tcp_flags = RTE_TCP_FIN_FLAG | RTE_TCP_SYN_FLAG | RTE_TCP_RST_FLAG | RTE_TCP_PSH_FLAG | RTE_TCP_ACK_FLAG | RTE_TCP_URG_FLAG | RTE_TCP_ECE_FLAG | RTE_TCP_CWR_FLAG;
		tcp_hdr->rx_win = rte_cpu_to_be_16(65535);
		tcp_hdr->cksum = 0;
		tcp_hdr->tcp_urp = 0;

		// 计算 TCP 校验和
		tcp_hdr->cksum = rte_ipv4_udptcp_cksum(ip_hdr, tcp_hdr);
	}

	return tx_bytes;
	return mutated;
}

uint64_t mutation_tcp_window(void **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode*)data;
	uint64_t tx_bytes = 0, mutated = 0;

	// L2 + L3 + L4 头部空间
	uint16_t pkt_size = sizeof(struct rte_ether_hdr) +
		sizeof(struct rte_ipv4_hdr) +
		sizeof(struct rte_tcp_hdr);

	for (unsigned int i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);

		char *pkt_data = rte_pktmbuf_append(m, pkt_size);
		if (!pkt_data) {
			continue;
		}

		// Ethernet header
		struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)pkt_data;
		struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
		struct rte_tcp_hdr *tcp_hdr = (struct rte_tcp_hdr *)(ip_hdr + 1);

		// 设置 MAC 地址（示例）
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.src, &eth_hdr->src_addr);
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.dst, &eth_hdr->dst_addr);
		eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

		// IPv4 header
		ip_hdr->version_ihl = RTE_IPV4_VHL_DEF;
		ip_hdr->type_of_service = 0;
		ip_hdr->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_tcp_hdr));
		ip_hdr->packet_id = rte_cpu_to_be_16(1);
		ip_hdr->fragment_offset = 0;
		ip_hdr->time_to_live = 225; // 64;
		ip_hdr->next_proto_id = IPPROTO_TCP;
		ip_hdr->src_addr = RANDOM_IP_SRC(cnode);
		ip_hdr->dst_addr = RANDOM_IP_DST(cnode);
		ip_hdr->hdr_checksum = 0;
		ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);

		// TCP header
		tcp_hdr->src_port = RANDOM_TCP_SRC(cnode);
		tcp_hdr->dst_port = RANDOM_TCP_DST(cnode);
		tcp_hdr->sent_seq = rte_cpu_to_be_32(rte_rdtsc() & (uint32_t)-1);
		tcp_hdr->recv_ack = 1;
		tcp_hdr->data_off = ((sizeof(struct rte_tcp_hdr) / 4) << 4);
		tcp_hdr->tcp_flags = RTE_TCP_ACK_FLAG | RTE_TCP_ACK_FLAG;
		/* mutation */
		tcp_hdr->rx_win = 0;
		tcp_hdr->cksum = 0;
		tcp_hdr->tcp_urp = 0;

		// 计算 TCP 校验和
		tcp_hdr->cksum = rte_ipv4_udptcp_cksum(ip_hdr, tcp_hdr);
	}

	return tx_bytes;
	return mutated;
}

uint64_t mutation_tcp_cksum(void **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode*)data;
	uint64_t tx_bytes = 0, mutated = 0;

	// L2 + L3 + L4 头部空间
	uint16_t pkt_size = sizeof(struct rte_ether_hdr) +
		sizeof(struct rte_ipv4_hdr) +
		sizeof(struct rte_tcp_hdr);

	for (unsigned int i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);

		char *pkt_data = rte_pktmbuf_append(m, pkt_size);
		if (!pkt_data) {
			continue;
		}

		// Ethernet header
		struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)pkt_data;
		struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
		struct rte_tcp_hdr *tcp_hdr = (struct rte_tcp_hdr *)(ip_hdr + 1);

		// 设置 MAC 地址（示例）
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.src, &eth_hdr->src_addr);
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.dst, &eth_hdr->dst_addr);
		eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

		// IPv4 header
		ip_hdr->version_ihl = RTE_IPV4_VHL_DEF;
		ip_hdr->type_of_service = 0;
		ip_hdr->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_tcp_hdr));
		ip_hdr->packet_id = rte_cpu_to_be_16(1);
		ip_hdr->fragment_offset = 0;
		ip_hdr->time_to_live = 225; // 64;
		ip_hdr->next_proto_id = IPPROTO_TCP;
		ip_hdr->src_addr = RANDOM_IP_SRC(cnode);
		ip_hdr->dst_addr = RANDOM_IP_DST(cnode);
		ip_hdr->hdr_checksum = 0;
		ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);

		// TCP header
		tcp_hdr->src_port = RANDOM_TCP_SRC(cnode);
		tcp_hdr->dst_port = RANDOM_TCP_DST(cnode);
		tcp_hdr->sent_seq = rte_cpu_to_be_32(rte_rdtsc() & (uint32_t)-1);
		tcp_hdr->recv_ack = 1;
		tcp_hdr->data_off = ((sizeof(struct rte_tcp_hdr) / 4) << 4);
		tcp_hdr->tcp_flags = RTE_TCP_ACK_FLAG | RTE_TCP_ACK_FLAG;
		tcp_hdr->rx_win = rte_cpu_to_be_16(65535);
		/* mutation */
		tcp_hdr->cksum = 1;
		tcp_hdr->tcp_urp = 0;

		// 计算 TCP 校验和
		/* mutation */
		// tcp_hdr->cksum = rte_ipv4_udptcp_cksum(ip_hdr, tcp_hdr);
	}

	return tx_bytes;
	return mutated;
}

uint64_t mutation_udp_len(void **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode*)data;
	uint64_t tx_bytes = 0, mutated = 0;
	const char *payload = "mutation udp length.";
	const uint16_t payload_len = strlen(payload) + 1;
	const uint16_t udp_len = sizeof(struct rte_udp_hdr) + payload_len;
	const uint16_t ipv4_len = sizeof(struct rte_ipv4_hdr) + udp_len;
	const uint16_t pkt_len = sizeof(struct rte_ether_hdr) + ipv4_len;

	for (unsigned int i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);

		char *pkt_data = rte_pktmbuf_append(m, pkt_len);
		if (!pkt_data) {
			continue;
		}

		// 填充 Ethernet
		struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)pkt_data;
		/* use inner's mac address */
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.src, &eth_hdr->src_addr);
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.dst, &eth_hdr->dst_addr);
		eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

		// 填充 IPv4
		struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
		memset(ip_hdr, 0, sizeof(*ip_hdr));
		ip_hdr->version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);
		ip_hdr->type_of_service = 0;
		ip_hdr->total_length = rte_cpu_to_be_16(ipv4_len);
		ip_hdr->packet_id = rte_cpu_to_be_16(1);
		ip_hdr->fragment_offset = 0;
		ip_hdr->time_to_live = 225; // 64;
		ip_hdr->next_proto_id = IPPROTO_UDP;
		// FIXME
		ip_hdr->src_addr = RANDOM_IP_SRC(cnode);
		ip_hdr->dst_addr = RANDOM_IP_DST(cnode);
		ip_hdr->hdr_checksum = 0;
		ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);

		/* ---------------- UDP Header ---------------- */
		struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip_hdr + 1);
		udp->src_port = RANDOM_UDP_SRC(cnode);
		udp->dst_port = RANDOM_UDP_DST(cnode);
		/* mutation */
		udp->dgram_len = rte_cpu_to_be_16(udp_len);
		udp->dgram_cksum = 0;

		/* ---------------- Payload ---------------- */
		if (payload && payload_len) {
			rte_memcpy((char *)(udp + 1), payload, payload_len);
		}

		mutated++;
		tx_bytes += pkt_len;
	}

	return tx_bytes;
	return mutated;
}

uint64_t mutation_udp_cksum(void **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode*)data;
	uint64_t tx_bytes = 0, mutated = 0;
	const char *payload = "cksum";
	const uint16_t payload_len = strlen(payload) + 1;
	const uint16_t udp_len = sizeof(struct rte_udp_hdr) + payload_len;
	const uint16_t ipv4_len = sizeof(struct rte_ipv4_hdr) + udp_len;
	const uint16_t pkt_len = sizeof(struct rte_ether_hdr) + ipv4_len;

	for (unsigned int i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);

		char *pkt_data = rte_pktmbuf_append(m, pkt_len);
		if (!pkt_data) {
			continue;
		}

		// 填充 Ethernet
		struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)pkt_data;
		/* use inner's mac address */
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.src, &eth_hdr->src_addr);
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.dst, &eth_hdr->dst_addr);
		eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

		// 填充 IPv4
		struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
		memset(ip_hdr, 0, sizeof(*ip_hdr));
		ip_hdr->version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);
		ip_hdr->type_of_service = 0;
		ip_hdr->total_length = rte_cpu_to_be_16(ipv4_len);
		ip_hdr->packet_id = rte_cpu_to_be_16(1);
		ip_hdr->fragment_offset = 0;
		ip_hdr->time_to_live = 225; // 64;
		ip_hdr->next_proto_id = IPPROTO_UDP;
		// FIXME
		ip_hdr->src_addr = RANDOM_IP_SRC(cnode);
		ip_hdr->dst_addr = RANDOM_IP_DST(cnode);
		ip_hdr->hdr_checksum = 0;
		ip_hdr->hdr_checksum = rte_ipv4_cksum(ip_hdr);

		/* ---------------- UDP Header ---------------- */
		struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip_hdr + 1);
		udp->src_port = RANDOM_UDP_SRC(cnode);
		udp->dst_port = RANDOM_UDP_DST(cnode);
		udp->dgram_len = rte_cpu_to_be_16(udp_len);
		/* mutation */
		udp->dgram_cksum = 0xffff;

		/* ---------------- Payload ---------------- */
		if (payload && payload_len) {
			rte_memcpy((char *)(udp + 1), payload, payload_len);
		}

		mutated++;
		tx_bytes += pkt_len;
	}

	return tx_bytes;
	return mutated;
}

#define SIZEOF_VXLAN (sizeof(struct rte_ether_hdr) + \
		sizeof(struct rte_ipv4_hdr) + \
		sizeof(struct rte_udp_hdr) + \
		8 \
		)
uint64_t mutation_udp_vxlan(void **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode*)data;
	uint64_t tx_bytes = 0, mutated = 0;
	const char *payload = "mutation vxlan";
	const uint16_t payload_len = strlen(payload) + 1;
	const uint16_t udp_len = sizeof(struct rte_udp_hdr) + payload_len;
	const uint16_t ipv4_len = sizeof(struct rte_ipv4_hdr) + udp_len;
	const uint16_t pkt_len = sizeof(struct rte_ether_hdr) + ipv4_len;

	for (unsigned int i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);

		char *pkt_data = rte_pktmbuf_append(m, pkt_len);
		if (!pkt_data) {
			continue;
		}

		// 填充 Ethernet
		struct rte_ether_hdr *eth_hdr = (struct rte_ether_hdr *)pkt_data;
		/* use inner's mac address */
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->vxlan.ether.src, &eth_hdr->src_addr);
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->vxlan.ether.dst, &eth_hdr->dst_addr);
		eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

		// 填充 IPv4
		struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
		memset(ip_hdr, 0, sizeof(*ip_hdr));
		ip_hdr->version_ihl = (4 << 4) | (sizeof(struct rte_ipv4_hdr) / 4);
		ip_hdr->type_of_service = 0;
		ip_hdr->total_length = rte_cpu_to_be_16(ipv4_len);
		ip_hdr->packet_id = rte_cpu_to_be_16(1);
		ip_hdr->fragment_offset = 0;
		ip_hdr->time_to_live = 225; // 64;
		ip_hdr->next_proto_id = IPPROTO_UDP;
		// FIXME
		ip_hdr->src_addr = RANDOM_IP_SRC(cnode);
		ip_hdr->dst_addr = RANDOM_IP_DST(cnode);
		ip_hdr->hdr_checksum  = 0;
		ip_hdr->hdr_checksum  = rte_ipv4_cksum(ip_hdr);

		/* ---------------- UDP Header ---------------- */
		struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip_hdr + 1);
		udp->src_port = RANDOM_UDP_SRC(cnode);
		udp->dst_port = RANDOM_UDP_DST(cnode);
		udp->dgram_len = rte_cpu_to_be_16(udp_len);
		/* mutation */
		udp->dgram_cksum = 0xffff;

		/* ---------------- Payload ---------------- */
		if (payload && payload_len) {
			rte_memcpy((char *)(udp + 1), payload, payload_len);
		}

		/* VXLAN */
		char *data = rte_pktmbuf_prepend(m, SIZEOF_VXLAN);
		if (!data) {
			rte_exit(EXIT_FAILURE,
					"Cannot rte_pktmbuf_prepend(%p %lu) rte_pktmbuf_headroom(m) %u\n",
					m, SIZEOF_VXLAN, rte_pktmbuf_headroom(m));
			return -1;
		}

		struct rte_ether_hdr *oeth = (struct rte_ether_hdr *)data;
		struct rte_ipv4_hdr *oip = (struct rte_ipv4_hdr *)(oeth + 1);
		struct rte_udp_hdr *oudp = (struct rte_udp_hdr *)(oip + 1);
		uint8_t *vxlan_hdr = (uint8_t *)(oudp + 1);

		/* 外层 L2 */
		/* use vxlan's mac address */
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->vxlan.ether.src, &oeth->src_addr);
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->vxlan.ether.dst, &oeth->dst_addr);
		oeth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

		/* 外层 L3 */
		oip->version_ihl = 0x45;
		oip->type_of_service = 0;
		// oip->total_length = rte_cpu_to_be_16(m->pkt_len - sizeof(struct rte_ether_hdr));
		oip->total_length = rte_cpu_to_be_16(pkt_len + 8 + sizeof(struct rte_udp_hdr) + sizeof(struct rte_ipv4_hdr));
		oip->packet_id = 0;
		oip->fragment_offset = 0;
		oip->time_to_live = 225; // 64;
		oip->next_proto_id = IPPROTO_UDP;
		/* FIXME random */
		uint64_t addr_vni = RANDOM_VXLAN_IP_VNI(cnode);
		oip->src_addr = (uint32_t)addr_vni;
		oip->dst_addr = RANDOM_VXLAN_IP_DST(cnode);
		oip->hdr_checksum = rte_ipv4_cksum(oip);

		/* 外层 UDP */
		/* FIXME find a port that is not 4789 or any one in use */
		oudp->src_port = RANDOM_VXLAN_UDP_SRC(cnode);
		/* XXX */
		oudp->dst_port = -RANDOM_VXLAN_UDP_DST(cnode);
		oudp->dgram_len = rte_cpu_to_be_16(m->pkt_len - sizeof(struct rte_ether_hdr) - sizeof(struct rte_ipv4_hdr));
		oudp->dgram_cksum = 0;

		/* VXLAN header */
		vxlan_hdr[0] = 0x08;
		vxlan_hdr[1] = 0;
		vxlan_hdr[2] = 0;
		vxlan_hdr[3] = 0;
		/* set vni acording to `ip:vni' */
		uint32_t vni = addr_vni >> 32;
		vxlan_hdr[4] = (vni >> 16) & 0xFF;
		vxlan_hdr[5] = (vni >> 8) & 0xFF;
		vxlan_hdr[6] = (vni) & 0xFF;
		vxlan_hdr[7] = 0;

		mutated++;
		tx_bytes += pkt_len;
	}

	return tx_bytes;
	return mutated;
}

static struct Mutator mac_mutators[] = {
	{ "vlan",      mutation_mac_vlan },
	{ "multicast", mutation_mac_multicast },
	{ "ipv6", mutation_mac_ipv6 },
};

static struct Mutator arp_mutators[] = {
	{ "src", mutation_arp_src },
};

static struct Mutator ip_mutators[] = {
	{ "version", mutation_ip_version },
	{ "ihl" , mutation_ip_ihl },
	{ "dscp" , mutation_ip_dscp },
	{ "ecn" , mutation_ip_ecn },
	{ "total_length" , mutation_ip_total_length },
	{ "id" , mutation_ip_id },
	{ "flags" , mutation_ip_flags },
	{ "fragment_offset", mutation_ip_fragment_offset },
	{ "ttl" , mutation_ip_ttl },
	{ "protocol" , mutation_ip_protocol },
	{ "cksum" , mutation_ip_cksum },
	{ "ipsec" , mutation_ip_ipsec },
};

static struct Mutator icmp_mutators[] = {
	{ "type", mutation_icmp_type },
};

static struct Mutator tcp_mutators[] = {
	{ "syn_flood", mutation_tcp_syn_flood },
	{ "data_off", mutation_tcp_data_off },
	{ "flags", mutation_tcp_flags },
	{ "window", mutation_tcp_window },
	{ "cksum", mutation_tcp_cksum },
};

static struct Mutator udp_mutators[] = {
	{ "len", mutation_udp_len },
	{ "cksum", mutation_udp_cksum },
	{ "vxlan", mutation_udp_vxlan },
};

#endif
