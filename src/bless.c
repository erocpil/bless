#include "bless.h"

#define SIP RTE_IPV4(10, 0, 0, 1)
#define SIP_OFFSET 10000

#define DIP RTE_IPV4(192, 168, 0, 1)
#define DIP_OFFSET 10000

#define PORT 9000
#define PORT_OFFSET 10000

int bless_parse_port_range(char *data, uint16_t *port, int32_t *range)
{
	if (!data || strlen(data) < 1) {
		return -1;
	}

	int i = 0;
	char *port_range = strdup(data);

	while (port_range[i] != '+') {
		i++;
	}

	if ('+' == port_range[i]) {
		port_range[i] = '\0';
		*range = atoi(&port_range[++i]);
		printf("%d\n", *range);
	} else {
		*range = 0;
	}

	*port = atoi(port_range);

	free(port_range);
	return 0;
}

int bless_parse_ip_range(char *data, uint32_t *ip, int64_t *range)
{
	if (!data || strlen(data) < 8) {
		return -1;
	}

	int i = 0;
	char *ip_range = strdup(data);

	while (ip_range[i] != '\0' && ip_range[i] != '+' ) {
		i++;
	}

	/* no range */
	if ('\0' == ip_range[i]) {
		*range = atoi(ip_range);
	}

	if ('+' == ip_range[i]) {
		ip_range[i] = '\0';
		*range = atoi(&ip_range[++i]);
		printf("%ld\n", *range);
	} else {
		*range = 0;
	}

	if (inet_pton(AF_INET, ip_range, ip) != 1) {
		free(ip_range);
		return -1;
	}

	free(ip_range);
	return 0;
}

int64_t bless_seperate_ip_range(char *ip_range)
{
	int i = 0;

	while (ip_range[i] != '\0') {
		if ( '+' == ip_range[i]) {
			ip_range[i] = '\0';
			return atoi(&ip_range[i + 1]);
		}
		i++;
	}

	return 0;
}

int32_t bless_seperate_port_range(char *port_range)
{
	int i = 0;

	while (port_range[i] != '\0') {
		if ( '+' == port_range[i]) {
			port_range[i] = '\0';
			printf(">> %s %s\n", port_range, &port_range[i + 1]);
			return atoi(&port_range[i + 1]);
		}
		i++;
	}

	return 0;
}

uint64_t bless_mbufs_udp(struct rte_mbuf **mbufs, unsigned int n, void *data)
{
	struct rte_ether_hdr *eth;
	struct rte_ipv4_hdr *ip;
	struct rte_udp_hdr *udp;
	const char *payload = "normal udp payload.";
	uint16_t payload_len = strlen(payload) + 1;
	uint64_t tx_bytes = 0;
	Cnode *cnode = (Cnode*)data;

	const uint16_t l2_len = sizeof(struct rte_ether_hdr);
	const uint16_t l3_len = sizeof(struct rte_ipv4_hdr);
	const uint16_t l4_len = sizeof(struct rte_udp_hdr);
	const uint16_t total_pkt_size = l2_len + l3_len + l4_len + payload_len;
	// const uint16_t total_pkt_size = l2_len + l3_len + l4_len + payload_len; // + cnode->ether.mtu;

	for (int i = 0; i < (int)n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);
		// 给 mbuf 留出空间
		assert(m->data_len == 0);
		assert(m->pkt_len == 0);
		if (!rte_pktmbuf_append(m, total_pkt_size)) {
			return -1;
		}
		tx_bytes += m->data_len;

		eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.src, &eth->src_addr);
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.dst, &eth->dst_addr);
		eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

		ip = (struct rte_ipv4_hdr *)(eth + 1);
		ip->version_ihl = 0x45;
		ip->type_of_service = 0;
		ip->total_length = rte_cpu_to_be_16(l3_len + l4_len + payload_len);
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
			printf("_");
			ip->hdr_checksum = rte_ipv4_cksum(ip);
		}

		udp = (struct rte_udp_hdr *)(ip + 1);
		udp->src_port = RANDOM_UDP_SRC(cnode);
		udp->dst_port = RANDOM_UDP_DST(cnode);
		udp->dgram_len = htons(sizeof(struct rte_udp_hdr) + payload_len);
		udp->dgram_cksum = 0;
		rte_memcpy((uint8_t *)udp + l4_len, payload, payload_len);

		if (OFFLOAD_UDP(cnode)) {
			m->ol_flags |= RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_UDP_CKSUM;
		} else {
			printf(".");
			udp->dgram_cksum = rte_ipv4_udptcp_cksum(ip, udp);
		}

		m->l2_len = l2_len;
		m->l3_len = l3_len;
		m->l4_len = l4_len;
	}
	return tx_bytes;
}

/*
 * TODO
 * 1. random pkts generation according to conf->bless
 * 2. set pkt type using rte_mbuf_dynfield detailed metrics
 */
uint64_t bless_mbufs_arp(struct rte_mbuf **mbufs, unsigned int n, void *data)
{
	uint64_t tx_bytes = 0;
	const uint16_t total_pkt_size = sizeof(struct rte_ether_hdr) + sizeof(struct rte_arp_hdr);
	Cnode *cnode = (Cnode*)data;

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

		struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
		struct rte_arp_hdr *arp_hdr = (struct rte_arp_hdr *)(eth_hdr + 1);

		static const struct rte_ether_addr dst_mac = {
			{ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }
		};
		static uint64_t flag = 1;
		if (flag++ & 1) {
			rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.src, &eth_hdr->src_addr);
			rte_ether_addr_copy(&dst_mac, &eth_hdr->dst_addr);
			eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP);

			arp_hdr->arp_hardware = rte_cpu_to_be_16(RTE_ARP_HRD_ETHER);
			arp_hdr->arp_protocol = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
			arp_hdr->arp_hlen = RTE_ETHER_ADDR_LEN;
			arp_hdr->arp_plen = sizeof(uint32_t);
			arp_hdr->arp_opcode = rte_cpu_to_be_16(RTE_ARP_OP_REQUEST);
			rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.src, &arp_hdr->arp_data.arp_sha);
			arp_hdr->arp_data.arp_sip = RANDOM_IP_SRC(cnode);
			rte_ether_addr_copy(&dst_mac, &arp_hdr->arp_data.arp_tha);
			arp_hdr->arp_data.arp_tip = RANDOM_IP_DST(cnode);
		} else {
			rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.src, &eth_hdr->src_addr);
			rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.dst, &eth_hdr->dst_addr);
			eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP);

			arp_hdr->arp_hardware = rte_cpu_to_be_16(RTE_ARP_HRD_ETHER);
			arp_hdr->arp_protocol = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
			arp_hdr->arp_hlen = RTE_ETHER_ADDR_LEN;
			arp_hdr->arp_plen = sizeof(uint32_t);
			arp_hdr->arp_opcode = rte_cpu_to_be_16(RTE_ARP_OP_REPLY);
			rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.dst, &arp_hdr->arp_data.arp_sha);
			arp_hdr->arp_data.arp_sip = RANDOM_IP_DST(cnode);
			rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.src, &arp_hdr->arp_data.arp_tha);
			arp_hdr->arp_data.arp_tip = RANDOM_IP_SRC(cnode);
		}
	}

	return tx_bytes;
}

uint64_t bless_mbufs_tcp(struct rte_mbuf **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode*)data;
	const char *payload = cnode->ether.type.ipv4.tcp.payload;
	uint16_t payload_len = cnode->ether.type.ipv4.tcp.payload_len;

	const uint16_t l2_len = sizeof(struct rte_ether_hdr);
	const uint16_t l3_len = sizeof(struct rte_ipv4_hdr);
	const uint16_t l4_len = sizeof(struct rte_tcp_hdr);
	const uint16_t total_pkt_size = l2_len + l3_len + l4_len + payload_len;
	// const uint16_t total_pkt_size = l2_len + l3_len + l4_len + payload_len; // + cnode->ether.mtu;

	uint64_t tx_bytes = 0;

	for (int i = 0; i < (int)n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);
		// 设置 mbuf 大小
		assert(m->data_len == 0);
		assert(m->pkt_len == 0);
		if (!rte_pktmbuf_append(m, total_pkt_size)) {
			return -1;
		}

		struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.src, &eth_hdr->src_addr);
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.dst, &eth_hdr->dst_addr);
		eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

		struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth_hdr + 1);
		memset(ip, 0, sizeof(*ip));
		ip->version_ihl = (4u << 4) | (sizeof(struct rte_ipv4_hdr) >> 2);
		ip->type_of_service = 0;
		ip->total_length = rte_cpu_to_be_16(l3_len + l4_len + payload_len);
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

		static uint32_t seq = 0, ack = 0;
		/* PSH | ACK */
		tcp->sent_seq = rte_cpu_to_be_32(seq++);
		tcp->recv_ack = rte_cpu_to_be_32(ack++);
		tcp->data_off = (sizeof(struct rte_tcp_hdr) / 4) << 4;
		tcp->tcp_flags = RTE_TCP_ACK_FLAG | RTE_TCP_PSH_FLAG;
		tcp->rx_win = rte_cpu_to_be_16(65535);

		if (payload && payload_len) {
			rte_memcpy((uint8_t *)tcp + l4_len, payload, payload_len);
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

#if 0
/* 计算 16-bit one's complement checksum（标准实现） */
static uint16_t calc_cksum(const void *buf, size_t len)
{
	const uint8_t *data = buf;
	uint32_t sum = 0;

	while (len > 1) {
		sum += (uint16_t)((data[0] << 8) | data[1]);
		data += 2;
		len -= 2;
	}

	if (len == 1) {
		sum += (uint16_t)(data[0] << 8);
	}

	/* add carry */
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);

	return (uint16_t)~sum;
}
#endif

uint64_t bless_mbufs_icmp(struct rte_mbuf **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode*)data;
	uint64_t tx_bytes = 0;
	const char *payload = cnode->ether.type.ipv4.icmp.payload;
	const uint16_t payload_len = cnode->ether.type.ipv4.icmp.payload_len;
	struct rte_ether_hdr *eth;
	struct rte_ipv4_hdr *ip;
	const uint16_t l2_len = sizeof(struct rte_ether_hdr);
	const uint16_t l3_len = sizeof(struct rte_ipv4_hdr);
	const uint16_t l4_len = sizeof(struct rte_icmp_hdr);
	const uint16_t total_pkt_size = l2_len + l3_len + l4_len + payload_len;
	// const uint16_t total_pkt_size = l2_len + l3_len + l4_len + payload_len; // + cnode->ether.mtu;

	for (int i = 0; i < (int)n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);
		// 给 mbuf 留出空间
		assert(m->data_len == 0);
		assert(m->pkt_len == 0);
		if (!rte_pktmbuf_append(m, total_pkt_size)) {
			return -1;
		}
		tx_bytes += total_pkt_size;

		eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.src, &eth->src_addr);
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.dst, &eth->dst_addr);
		eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

		ip = (struct rte_ipv4_hdr *)(eth + 1);
		ip->version_ihl = 0x45;
		ip->type_of_service = 0;
		ip->total_length = rte_cpu_to_be_16(l3_len + l4_len + payload_len);
		ip->packet_id = htons((uint16_t)rte_rand());
		ip->fragment_offset = 0;
		ip->time_to_live = 64;
		ip->next_proto_id = IPPROTO_ICMP;
		ip->src_addr = RANDOM_IP_SRC(cnode);
		ip->dst_addr = RANDOM_IP_DST(cnode);

		ip->hdr_checksum = 0;
		ip->hdr_checksum = rte_ipv4_cksum(ip);

		static uint32_t flag = 1;
		static uint16_t seq = 1;
		struct rte_icmp_hdr *icmp = (struct rte_icmp_hdr *)(ip + 1);
		if (flag++ & 1) {
			icmp->icmp_type = RTE_IP_ICMP_ECHO_REQUEST; // Echo Request
			seq++;
		} else {
			icmp->icmp_type = RTE_IP_ICMP_ECHO_REPLY; // Echo Reply
		}
		icmp->icmp_code = 0;
		icmp->icmp_ident = rte_cpu_to_be_16(0x1234);
		icmp->icmp_seq_nb   = rte_cpu_to_be_16(seq);

		if (payload) {
			rte_memcpy((char *)icmp + sizeof(*icmp), payload, payload_len);
		}

		/* checksum */
		icmp->icmp_cksum = 0;
		const uint16_t icmp_len = l4_len + payload_len;
		icmp->icmp_cksum = icmp_calc_cksum(icmp, icmp_len);
	}

	return tx_bytes;
}

uint64_t bless_mbufs_erroneous(struct rte_mbuf **mbufs, unsigned int n, void *data)
{
	return 0;
}

#define SIZEOF_VXLAN (sizeof(struct rte_ether_hdr) + \
		sizeof(struct rte_ipv4_hdr) + \
		sizeof(struct rte_udp_hdr) + \
		8 \
		)

uint64_t bless_encap_vxlan(struct rte_mbuf **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode*)data;

	for (int i = 0; i < (int)n; i++) {
		struct rte_mbuf *m = mbufs[i];

		char *data = rte_pktmbuf_prepend(m, SIZEOF_VXLAN);
		if (!data) {
			rte_exit(EXIT_FAILURE,
					"Cannot rte_pktmbuf_prepend(%p %lu) rte_pktmbuf_headroom(m) %u\n",
					m, SIZEOF_VXLAN, rte_pktmbuf_headroom(m));
			return -1;
		}

#if 0
		/* XXX */
		if (m->data_len > cnode->ether.mtu) {
			uint16_t frame_len1 = cnode->ether.mtu + sizeof(struct rte_ether_addr) * 2 + sizeof(uint16_t);
			m->data_len = frame_len1;
			m->pkt_len = frame_len1;
		}
#endif

		struct rte_ether_hdr *eth = (struct rte_ether_hdr *)data;
		struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
		struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip + 1);
		uint8_t *vxlan_hdr = (uint8_t *)(udp + 1);

		/* 外层 L2 */
		/* use inner's mac address */
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.src, &eth->src_addr);
		rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.dst, &eth->dst_addr);
		eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

		/* 外层 L3 */
		ip->version_ihl = 0x45;
		ip->type_of_service = 0;
		ip->total_length = rte_cpu_to_be_16(m->pkt_len - sizeof(struct rte_ether_hdr));
		ip->packet_id = 0;
		ip->fragment_offset = 0;
		ip->time_to_live = 64;
		ip->next_proto_id = IPPROTO_UDP;
		uint64_t addr_vni = RANDOM_VXLAN_IP_VNI(cnode);
		ip->src_addr = (uint32_t)addr_vni;
		ip->dst_addr = RANDOM_VXLAN_IP_DST(cnode);
		// ip->hdr_checksum = rte_ipv4_cksum(ip);
		ip->hdr_checksum = 0;

		/* 外层 UDP */
		udp->src_port = RANDOM_VXLAN_UDP_SRC(cnode);
		// udp->dst_port = RANDOM_VXLAN_UDP_DST(cnode); // rte_cpu_to_be_16(RTE_VXLAN_DEFAULT_PORT);
		udp->dst_port = rte_cpu_to_be_16(RTE_VXLAN_DEFAULT_PORT);
		udp->dgram_len = rte_cpu_to_be_16(m->pkt_len - sizeof(struct rte_ether_hdr) - sizeof(struct rte_ipv4_hdr));
		udp->dgram_cksum = 0;

		/* VXLAN header */
		vxlan_hdr[0] = 0x08;
		vxlan_hdr[1] = 0;
		vxlan_hdr[2] = 0;
		vxlan_hdr[3] = 0;
		uint32_t vni = addr_vni >> 32;
		// printf("vni %d\n", vni);
		vxlan_hdr[4] = (vni >> 16) & 0xFF;
		vxlan_hdr[5] = (vni >> 8) & 0xFF;
		vxlan_hdr[6] = (vni) & 0xFF;
		vxlan_hdr[7] = 0;

#if 1
		m->ol_flags = RTE_MBUF_F_TX_TUNNEL_VXLAN |
			// RTE_MBUF_F_TX_TUNNEL_UDP |
			RTE_MBUF_F_TX_OUTER_IPV4 |
			RTE_MBUF_F_TX_OUTER_IP_CKSUM |
			RTE_MBUF_F_TX_OUTER_UDP_CKSUM |
			RTE_MBUF_F_TX_IPV4 |
			RTE_MBUF_F_TX_IP_CKSUM |
			RTE_MBUF_F_TX_TCP_CKSUM |
			RTE_MBUF_F_TX_UDP_CKSUM;
#else
		m->ol_flags =
			RTE_MBUF_F_TX_OUTER_IPV4 |       // 外层IPv4
			RTE_MBUF_F_TX_OUTER_IP_CKSUM |
			RTE_MBUF_F_TX_OUTER_UDP_CKSUM |  // 外层UDP校验和
			RTE_MBUF_F_TX_IPV4 |             // 内层IPv4
			RTE_MBUF_F_TX_IP_CKSUM |         // 内层IP校验和
			RTE_MBUF_F_TX_UDP_CKSUM;         // 内层UDP校验和
#endif

		// inner rte_ipv4_phdr_cksum()
		// struct rte_ether_hdr *ethh = rte_pktmbuf_mtod_offset(m, struct rte_ether_hdr*, SIZEOF_VXLAN);
		struct rte_ipv4_hdr *iph = rte_pktmbuf_mtod_offset(m, struct rte_ipv4_hdr*, SIZEOF_VXLAN + sizeof(struct rte_ether_hdr));
		if (IPPROTO_UDP == iph->type_of_service) {
			struct rte_udp_hdr *udph = (struct rte_udp_hdr*)(iph + 1);
			udph->dgram_cksum = rte_ipv4_phdr_cksum((struct rte_ipv4_hdr*)iph, m->ol_flags);
		} else if (IPPROTO_TCP == iph->type_of_service) {
			struct rte_tcp_hdr *tcph = (struct rte_tcp_hdr*)(iph + 1);
			tcph->cksum = rte_ipv4_phdr_cksum((struct rte_ipv4_hdr*)iph, m->ol_flags);
		} else {
		}

		m->outer_l2_len = sizeof(struct rte_ether_hdr);
		m->outer_l3_len = sizeof(struct rte_ipv4_hdr);
	}

	return SIZEOF_VXLAN;
}

static uint64_t (*bless_get_funcs[])(struct rte_mbuf **mbufs, unsigned int n, void *data) = {
	bless_mbufs_arp,
	bless_mbufs_icmp,
	bless_mbufs_tcp,
	bless_mbufs_udp,
	bless_mbufs_erroneous,
	NULL
};

static uint64_t (*bless_encap_outer[])(struct rte_mbuf **mbufs, unsigned int n, void *data) = {
	bless_encap_vxlan,
	NULL
};

uint64_t bless_mbufs(struct rte_mbuf **mbufs, uint32_t n, enum BLESS_TYPE type, void *data)
{
	uint64_t r = 0;
	uint64_t tx_bytes  = 0;

	if (type < 0 || type >= TYPE_MAX) {
		return 0;
	}

	Cnode *cnode = (Cnode*)data;
	/* inner */
	r = bless_get_funcs[type](mbufs, n, cnode);
	if (!r) {
		return 0;
	}
	if (cnode->vxlan.enable) {
		/* outer */
		uint16_t ra = rdtsc16() & 1023;
		if ((ra % 100) < cnode->vxlan.ratio) {
			tx_bytes = bless_encap_outer[0](mbufs, n, cnode);
			if (!tx_bytes) {
				return 0;
			}
		} else {
			// printf("no vxlan\n");
			// rte_exit(EXIT_FAILURE, "[%s %d] no vxlan\n", __func__, __LINE__, n);
		}
	}

	return tx_bytes + r;
}

int bless_alloc_mbufs(struct rte_mempool *pktmbuf_pool, struct rte_mbuf **mbufs, int n)
{
	if (rte_pktmbuf_alloc_bulk(pktmbuf_pool, mbufs, n) != 0) {
		rte_exit(EXIT_FAILURE, "[%s %d] Cannot init mbuf(%d)\n", __func__, __LINE__, n);
		return -1;
	}
	return n;
}

struct rte_mempool *bless_create_pktmbuf_pool(uint32_t n, char *name)
{
	if (n >= (INT_MAX >> 1)) {
		rte_exit(EXIT_FAILURE, "Too many mbuf %u\n", n);
	}

	printf("creating pktmbufpool %s %u ...\n", name, n);
	struct rte_mempool *pktmbuf_pool = rte_pktmbuf_pool_create(name, n,
			0 /* MEMPOOL_CACHE_SIZE */, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
			rte_socket_id());
	if (!pktmbuf_pool) {
		rte_exit(EXIT_FAILURE, "Cannot init rte_pktmbuf_pool_create()\n");
	}

	// rte_mempool_dump(stdout, pktmbuf_pool);

	return pktmbuf_pool;
}

void dist_ratio_init(struct dist_ratio *dr)
{
	dr->num = 1;
	for (int i = 0; i < TYPE_MAX; i++) {
		dr->weight[i] = 0;
		dr->quota[i] = 0;
	}
}

struct bless_conf *bless_init(int argc, char *argv[])
{
	struct bless_conf *bconf = (struct bless_conf*)malloc(sizeof(struct bless_conf));
	if (!bconf) {
		return NULL;
	}
	memset(bconf, 0, sizeof(struct bless_conf));
	dist_ratio_init(&bconf->dist_ratio);

	return bconf;
}

static void distribute(uint32_t *weights, uint32_t n, uint64_t total, uint64_t *result)
{
	if (total < n) {
		fprintf(stderr, "总数太小，无法保证每个变量至少为 1\n");
		exit(1);
	}

	int sum = 0;
	// Step1: 基础分配，每个先分到1
	for (int i = 0; i < n; i++) {
		result[i] = 1;
		sum += weights[i];
	}
	total -= n; // 剩余可分配数量

	// Step2: 按比例分配剩余的
	double *temp = (double*)malloc(n * sizeof(double));
	double *frac = (double*)malloc(n * sizeof(double));
	if (!temp || !frac) {
		fprintf(stderr, "Memory allocation failed\n");
		exit(1);
	}

	uint64_t allocated = 0;
	for (int i = 0; i < n; i++) {
		temp[i] = (double)total * (weights[i] / sum);
		int add = (int)temp[i];     // 整数部分
		result[i] += add;
		frac[i] = temp[i] - add;    // 小数部分
		allocated += add;
	}

	// Step3: 把剩下的分配给小数部分最大的
	int remain = total - allocated;
	printf("total %lu allocated %lu remain %d\n", total, allocated, remain);
	while (remain > 0) {
		int idx = 0;
		for (int i = 1; i < n; i++) {
			if (frac[i] > frac[idx]) idx = i;
		}
		result[idx]++;
		frac[idx] = -1; // 标记已处理
		remain--;
	}
	printf("total %lu allocated %lu remain %d\n", total, allocated, remain);

	free(temp);
	free(frac);
}

int bless_set_dist(struct bless_conf* bconf, struct dist_ratio *ratio, struct bless_encap_params *bep)
{
	uint32_t weight[TYPE_MAX - 1] = { 0 };
	int n = 0;
	uint64_t *result = NULL;

	for (int i = 0; i < TYPE_MAX - 1; i++) {
		if (ratio->weight[i] > 0) {
			weight[n++] = ratio->weight[i];
		}
	}
	result = (uint64_t*)malloc(sizeof(uint64_t) * n);
	if (bconf->size) {
		if (!n) {
			return -EPERM;
		} else {
			distribute(weight, n, bconf->size, result);
			for (int i = 0, n = 0; i < TYPE_MAX - 1; i++) {
				if (ratio->weight[i] > 0) {
					printf("-> ");
					ratio->quota[i] = result[n++];
				}
				printf("%s \t\t%u\n", BLESS_TYPE_STR[i], ratio->quota[i]);
			}
		}
	} else {
		for (int i = 0; i < TYPE_MAX - 1; i++) {
			ratio->quota[i] = ratio->weight[i];
			printf("%s \t\t%lu\n", BLESS_TYPE_STR[i], result[n]);
		}
	}
	ratio->quota[TYPE_MAX - 1] = ratio->weight[TYPE_MAX - 1];
	printf("%s \t%u\n", BLESS_TYPE_STR[TYPE_MAX - 1], ratio->quota[TYPE_MAX - 1]);
	memcpy(&bconf->dist_ratio, ratio, sizeof(*ratio));

	uint32_t capacity = bconf->dist_ratio.num;
	uint32_t size = 0;
	if (capacity > (1 << 16)) {
		size = 1 << 16;
		capacity = size;
	} else {
		size = make_power_of_2(capacity);
	}
	struct distribution *dist =
		rte_malloc(NULL, sizeof(struct distribution) + sizeof(int32_t) * size, 0);
	if (!dist) {
		rte_exit(EXIT_FAILURE, "Cannot rte_malloc(distribution)\n");
	}
	dist->size = size;

	distribute(weight, n, size, result);
	for (int i = 0; i < n; i++) {
		printf("%d %lu\n", i, result[i]);
	}
	printf("\n");
	for (int i = 0, pos = 0, q = 0; i < TYPE_MAX - 1; i++) {
		if (bconf->dist_ratio.weight[i] <= 0) {
			continue;
		}
		printf("type %d %s\n", q, BLESS_TYPE_STR[i]);
		for (int j = 0; j < result[q]; j++) {
			dist->data[pos++] = i;
		}
		q++;
	}
	free(result);

	printf("=> size %u\n", size);
	dist->capacity = capacity;
	dist->mask = size - 1;
	bconf->dist = dist;

	bconf->bep.inner = bep->inner;
	bconf->bep.outer = bep->outer;
	getchar();

	// DISTRIBUTION_DUMP(bconf->dist);

	return 0;
}

int32_t bless_parse_type(enum BLESS_TYPE type, char *optarg)
{
	char *end = NULL;
	int64_t n = 0;

	if (!optarg) {
		printf("INT_MIN %d\n", INT_MIN);
		return INT_MIN;
	} else if ('-' == optarg[0]) {
		goto ERROR;
	}

	if (TYPE_MAX == type) {
		uint64_t n = strtoul(optarg, &end, 0);
		printf("total packets ");
		if ('0' == optarg[0] || '\0' == optarg[0] || !end || (*end != '\0')) {
			printf(" => inf ");
			n = 0;
		}
		printf("%lu\n", n);
		return n;
	}

	n = strtoul(optarg, &end, 0);
	if (n <= INT_MIN / 2 || n >= INT_MAX / 2) {
		goto ERROR;
	}
	if ('\0' == optarg[0] || !end || (*end != '\0')) {
		printf(" => ");
		n = 0;
	} else if (n > INT_MAX / TYPE_MAX) {
		n = INT_MAX / TYPE_MAX - 1;
		printf(" => ");
	}
	printf("%lu\n", n);

	if (TYPE_ERRONEOUS == type && n > 100) {
		goto ERROR;
	}

	return n;

ERROR:
	rte_exit(EXIT_FAILURE, "Invalid bless arguments a `%s'\n", optarg);
}

void bless_print_mac(const struct rte_ether_addr *mac)
{
	char buf[RTE_ETHER_ADDR_FMT_SIZE];
	rte_ether_format_addr(buf, sizeof(buf), mac);
	printf("MAC: %s\n", buf);
}

void bless_print_ipv4(uint32_t ip)
{
	struct in_addr in;
	in.s_addr = ip;
	char buf[INET_ADDRSTRLEN];
	if (inet_ntop(AF_INET, &in, buf, sizeof(buf)) != NULL) {
		printf("IP: %s\n", buf);
	}
}
