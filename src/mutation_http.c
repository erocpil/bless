#include "bless.h"
#include "http.h"
#include <rte_tcp.h>

typedef uint64_t (*mutation_func)(void **mbufs, unsigned int n, void *data);
struct Mutator {
	char name[32];
	mutation_func func;
};

static inline void offload_ip4(struct rte_mbuf *m, struct rte_ipv4_hdr *iph)
{
	iph->hdr_checksum = 0;
	m->ol_flags |= RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM;
}

static inline void offload_tcp(struct rte_mbuf *m, struct rte_ipv4_hdr *iph,
	struct rte_tcp_hdr *tcph)
{
	tcph->cksum = 0;
	m->ol_flags |= RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_TCP_CKSUM;
}

/**
 * @file mutation_http.c
 * @brief HTTP protocol mutations.
 *
 * Reconstructs HTTP request packets with deliberately malformed
 * request lines, headers, and framing to stress-test L7 DPI engines,
 * WAFs, and load balancers.
 */

static uint16_t write_tcp_wrapper(struct rte_mbuf *m, Cnode *cnode,
	uint16_t payload_len, uint16_t src_port, uint16_t dst_port)
{
	const uint16_t l2_len = sizeof(struct rte_ether_hdr);
	const uint16_t l3_len = sizeof(struct rte_ipv4_hdr);
	const uint16_t l4_len = sizeof(struct rte_tcp_hdr);
	const uint16_t pkt_len = l2_len + l3_len + l4_len + payload_len;

	char *pkt_data = rte_pktmbuf_append(m, pkt_len);
	if (!pkt_data) {
		return 0;
	}

	struct rte_ether_hdr *eth = (struct rte_ether_hdr *)pkt_data;
	rte_ether_addr_copy((struct rte_ether_addr *)cnode->ether.src, &eth->src_addr);
	rte_ether_addr_copy((struct rte_ether_addr *)cnode->ether.dst, &eth->dst_addr);
	eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

	struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
	memset(ip, 0, sizeof(*ip));
	ip->version_ihl = (4u << 4) | (sizeof(*ip) >> 2);
	ip->total_length = rte_cpu_to_be_16(l3_len + l4_len + payload_len);
	ip->packet_id = rte_cpu_to_be_16((uint16_t)fast_rand_next());
	ip->fragment_offset = rte_cpu_to_be_16(RTE_IPV4_HDR_DF_FLAG);
	ip->time_to_live = 64;
	ip->next_proto_id = IPPROTO_TCP;
	ip->src_addr = RANDOM_IP_SRC(cnode);
	ip->dst_addr = RANDOM_IP_DST(cnode);
	offload_ip4(m, ip);

	struct rte_tcp_hdr *tcp = (struct rte_tcp_hdr *)(ip + 1);
	memset(tcp, 0, sizeof(*tcp));
	tcp->src_port = rte_cpu_to_be_16(src_port);
	tcp->dst_port = rte_cpu_to_be_16(dst_port);
	tcp->sent_seq = rte_cpu_to_be_32(fast_rand_next());
	tcp->data_off = ((sizeof(*tcp) / 4) << 4) | RTE_TCP_PSH_FLAG | RTE_TCP_ACK_FLAG;
	tcp->tcp_flags = RTE_TCP_PSH_FLAG | RTE_TCP_ACK_FLAG;
	tcp->rx_win = rte_cpu_to_be_16(65535);
	offload_tcp(m, ip, tcp);

	m->l2_len = l2_len;
	m->l3_len = l3_len;
	m->l4_len = l4_len;
	return l3_len + l4_len;
}

/* mutation_http_malformed -- missing HTTP version in request line */
uint64_t mutation_http_malformed(void **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode *)data;
	uint64_t tx_bytes = 0;
	for (unsigned i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);

		char buf[256];
		int plen = snprintf(buf, sizeof(buf),
			"GET / HTTP/9.9\r\n"    /* invalid version */
			"Host: example.com\r\n"
			"\r\n");
		if (plen <= 0) {
			continue;
		}
		uint16_t off = write_tcp_wrapper(m, cnode, (uint16_t)plen,
			(uint16_t)(1024 + (fast_rand_next() & 0x7FFF)), 80);
		if (!off) {
			continue;
		}
		memcpy(rte_pktmbuf_mtod_offset(m, char *, off), buf, (uint16_t)plen);
		tx_bytes += off + (uint16_t)plen + sizeof(struct rte_ether_hdr);
	}
	return tx_bytes;
}

/* mutation_http_method_invalid -- nonsense HTTP method */
uint64_t mutation_http_method_invalid(void **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode *)data;
	uint64_t tx_bytes = 0;
	const char *methods[] = {"FLURB", "XXXX", "GETT", "\x00\x00\x00\x00"};
	for (unsigned i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);

		char buf[256];
		const char *method = methods[fast_rand_next() & 3];
		int plen = snprintf(buf, sizeof(buf),
			"%s / HTTP/1.1\r\n"
			"Host: example.com\r\n"
			"\r\n", method);
		if (plen <= 0) {
			continue;
		}
		uint16_t off = write_tcp_wrapper(m, cnode, (uint16_t)plen,
			(uint16_t)(1024 + (fast_rand_next() & 0x7FFF)), 80);
		if (!off) {
			continue;
		}
		memcpy(rte_pktmbuf_mtod_offset(m, char *, off), buf, (uint16_t)plen);
		tx_bytes += off + (uint16_t)plen + sizeof(struct rte_ether_hdr);
	}
	return tx_bytes;
}

/* mutation_http_host_overflow -- absurdly long Host header */
uint64_t mutation_http_host_overflow(void **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode *)data;
	uint64_t tx_bytes = 0;
	for (unsigned i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);

		char buf[512];
		/* Build a ~400-byte Host header (valid domain chars repeated) */
		char long_host[401];
		for (int j = 0; j < 400; j++)
			long_host[j] = (char)('a' + (fast_rand_next() & 0xF));
		long_host[400] = '\0';

		int plen = snprintf(buf, sizeof(buf),
			"GET / HTTP/1.1\r\n"
			"Host: %s\r\n"
			"\r\n", long_host);
		if (plen <= 0) {
			continue;
		}
		uint16_t off = write_tcp_wrapper(m, cnode, (uint16_t)plen,
			(uint16_t)(1024 + (fast_rand_next() & 0x7FFF)), 80);
		if (!off) {
			continue;
		}
		memcpy(rte_pktmbuf_mtod_offset(m, char *, off), buf, (uint16_t)plen);
		tx_bytes += off + (uint16_t)plen + sizeof(struct rte_ether_hdr);
	}
	return tx_bytes;
}

/* mutation_http_crlf_missing -- no CRLF between headers */
uint64_t mutation_http_crlf_missing(void **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode *)data;
	uint64_t tx_bytes = 0;
	for (unsigned i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);

		/* Missing \r\n after Host, just \n */
		char buf[256];
		int plen = snprintf(buf, sizeof(buf),
			"GET / HTTP/1.1\r\n"
			"Host: example.com\n"     /* only LF, no CR */
			"\r\n");
		if (plen <= 0) {
			continue;
		}
		uint16_t off = write_tcp_wrapper(m, cnode, (uint16_t)plen,
			(uint16_t)(1024 + (fast_rand_next() & 0x7FFF)), 80);
		if (!off) {
			continue;
		}
		memcpy(rte_pktmbuf_mtod_offset(m, char *, off), buf, (uint16_t)plen);
		tx_bytes += off + (uint16_t)plen + sizeof(struct rte_ether_hdr);
	}
	return tx_bytes;
}

struct Mutator http_mutators[] = {
	{ "malformed",      mutation_http_malformed },
	{ "method_invalid", mutation_http_method_invalid },
	{ "host_overflow",  mutation_http_host_overflow },
	{ "crlf_missing",   mutation_http_crlf_missing },
};
