#include "bless.h"
#include "dns.h"
#include <rte_udp.h>

/* struct Mutator is defined in mutation.h -- redeclare here to avoid
 * including mutation.h (which contains non-static function definitions
 * that would cause duplicate symbols at link time). */
typedef uint64_t (*mutation_func)(void **mbufs, unsigned int n, void *data);
struct Mutator {
	char name[32];
	mutation_func func;
};

/* Local helpers (duplicated from mutation.h to avoid include). */
static inline void offload_ip4(struct rte_mbuf *m, struct rte_ipv4_hdr *iph)
{
	iph->hdr_checksum = 0;
	m->ol_flags |= RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM;
}

/**
 * @file mutation_dns.c
 * @brief DNS protocol mutations.
 *
 * Reconstructs DNS packets with deliberately corrupted fields to
 * stress-test DNS parsers, DPI engines, and recursive resolvers.
 */

/* Helper: fill IP+UDP wrappers, returned l3_len+l4_len offset for DNS payload. */
static uint16_t fill_ip_udp(struct rte_mbuf *m, Cnode *cnode,
	uint16_t dns_total, uint16_t src_port, uint16_t dst_port)
{
	const uint16_t l2_len = sizeof(struct rte_ether_hdr);
	const uint16_t l3_len = sizeof(struct rte_ipv4_hdr);
	const uint16_t l4_len = sizeof(struct rte_udp_hdr);
	const uint16_t pkt_len = l2_len + l3_len + l4_len + dns_total;

	char *pkt_data = rte_pktmbuf_append(m, pkt_len);
	if (!pkt_data) {
		return 0;
	}

	struct rte_ether_hdr *eth = (struct rte_ether_hdr *)pkt_data;
	rte_ether_addr_copy((struct rte_ether_addr *)cnode->ether.src,
			    &eth->src_addr);
	rte_ether_addr_copy((struct rte_ether_addr *)cnode->ether.dst,
			    &eth->dst_addr);
	eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

	struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
	memset(ip, 0, sizeof(*ip));
	ip->version_ihl = (4u << 4) | (sizeof(*ip) >> 2);
	ip->total_length = rte_cpu_to_be_16(l3_len + l4_len + dns_total);
	ip->packet_id = rte_cpu_to_be_16((uint16_t)fast_rand_next());
	ip->fragment_offset = rte_cpu_to_be_16(RTE_IPV4_HDR_DF_FLAG);
	ip->time_to_live = 64;
	ip->next_proto_id = IPPROTO_UDP;
	ip->src_addr = RANDOM_IP_SRC(cnode);
	ip->dst_addr = RANDOM_IP_DST(cnode);
	offload_ip4(m, ip);

	struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip + 1);
	memset(udp, 0, sizeof(*udp));
	udp->src_port = rte_cpu_to_be_16(src_port);
	udp->dst_port = rte_cpu_to_be_16(dst_port);
	udp->dgram_len = rte_cpu_to_be_16(l4_len + dns_total);

	m->l2_len = l2_len;
	m->l3_len = l3_len;
	m->l4_len = l4_len;

	return l3_len + l4_len;
}

/* Default domain + wire-format encode */
static const char *def_name = "\x03www\x07example\x03com\x00";
#define DEF_NAME_LEN 16

static uint16_t build_dns_wire(char *buf, uint16_t max, const char *wname,
	uint16_t wname_len, uint16_t qtype, uint16_t flags)
{
	uint16_t total = DNS_HDR_SIZE + wname_len + DNS_QUESTION_TAIL;
	if (total > max) {
		return 0;
	}

	struct dns_hdr *dns = (struct dns_hdr *)buf;
	memset(dns, 0, sizeof(*dns));
	dns->id      = rte_cpu_to_be_16((uint16_t)fast_rand_next());
	dns->flags   = rte_cpu_to_be_16(flags);
	dns->qdcount = rte_cpu_to_be_16(1);

	memcpy(buf + DNS_HDR_SIZE, wname, wname_len);

	struct dns_question_tail *qt =
		(struct dns_question_tail *)(buf + DNS_HDR_SIZE + wname_len);
	qt->qtype  = rte_cpu_to_be_16(qtype);
	qt->qclass = rte_cpu_to_be_16(1);

	return total;
}

/* mutation_dns_tc -- set TC (truncated) flag, response bit, RCODE=0 */
uint64_t mutation_dns_tc(void **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode *)data;
	uint64_t tx_bytes = 0;
	for (unsigned i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);

		char payload[512];
		uint16_t plen = build_dns_wire(payload, sizeof(payload),
			def_name, DEF_NAME_LEN, 1, 0x8280); /* QR=1, TC=1, RD=1 */
		if (!plen) {
			continue;
		}

		uint16_t off = fill_ip_udp(m, cnode, plen,
			(uint16_t)(1024 + (fast_rand_next() & 0x7FFF)), 53);
		if (!off) {
			continue;
		}

		memcpy(rte_pktmbuf_mtod_offset(m, char *, off), payload, plen);
		tx_bytes += off + plen + sizeof(struct rte_ether_hdr);
	}
	return tx_bytes;
}

/* mutation_dns_nxdomain -- RCODE=3 (NXDOMAIN), QR=1 */
uint64_t mutation_dns_nxdomain(void **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode *)data;
	uint64_t tx_bytes = 0;
	for (unsigned i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);

		char payload[512];
		uint16_t plen = build_dns_wire(payload, sizeof(payload),
			def_name, DEF_NAME_LEN, 1, 0x8183); /* QR=1, RD=1, RCODE=3 */
		if (!plen) {
			continue;
		}

		uint16_t off = fill_ip_udp(m, cnode, plen,
			(uint16_t)(1024 + (fast_rand_next() & 0x7FFF)), 53);
		if (!off) {
			continue;
		}

		memcpy(rte_pktmbuf_mtod_offset(m, char *, off), payload, plen);
		tx_bytes += off + plen + sizeof(struct rte_ether_hdr);
	}
	return tx_bytes;
}

/* mutation_dns_qtype -- random QTYPE including invalid values 0, 65535 */
uint64_t mutation_dns_qtype_inv(void **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode *)data;
	uint64_t tx_bytes = 0;
	for (unsigned i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);

		/* 20% chance of invalid QTYPE (0, 251, 65535) */
		uint16_t qtype;
		uint32_t r = fast_rand_next() % 10;
		if (r < 2) {
			qtype = 0;
		} else if (r < 4) {
			qtype = 251; /* IXFR (rare) */
		} else if (r < 5) {
			qtype = 65535; /* out of range */
		} else {
			qtype = fast_rand_next() % 256;
		}

		char payload[512];
		uint16_t plen = build_dns_wire(payload, sizeof(payload),
			def_name, DEF_NAME_LEN, qtype, 0x0100);
		if (!plen) {
			continue;
		}

		uint16_t off = fill_ip_udp(m, cnode, plen,
			(uint16_t)(1024 + (fast_rand_next() & 0x7FFF)), 53);
		if (!off) {
			continue;
		}

		memcpy(rte_pktmbuf_mtod_offset(m, char *, off), payload, plen);
		tx_bytes += off + plen + sizeof(struct rte_ether_hdr);
	}
	return tx_bytes;
}

/* mutation_dns_label_overflow -- first label length byte set to >63 */
uint64_t mutation_dns_label_overflow(void **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode *)data;
	uint64_t tx_bytes = 0;
	for (unsigned i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);

		char payload[512];
		uint16_t plen = build_dns_wire(payload, sizeof(payload),
			def_name, DEF_NAME_LEN, 1, 0x0100);
		if (!plen) {
			continue;
		}

		/* Corrupt first label length: set to 64-255 (max allowed is 63) */
		uint8_t *label = (uint8_t *)(payload + DNS_HDR_SIZE);
		*label = 64 + (fast_rand_next() & 0x3F);

		uint16_t off = fill_ip_udp(m, cnode, plen,
			(uint16_t)(1024 + (fast_rand_next() & 0x7FFF)), 53);
		if (!off) {
			continue;
		}

		memcpy(rte_pktmbuf_mtod_offset(m, char *, off), payload, plen);
		tx_bytes += off + plen + sizeof(struct rte_ether_hdr);
	}
	return tx_bytes;
}

/* mutation_dns_id_zero -- zero transaction ID */
uint64_t mutation_dns_id_zero(void **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode *)data;
	uint64_t tx_bytes = 0;
	for (unsigned i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);

		char payload[512];
		uint16_t plen = build_dns_wire(payload, sizeof(payload),
			def_name, DEF_NAME_LEN, 1, 0x0100);
		if (!plen) {
			continue;
		}

		/* Zero transaction ID */
		struct dns_hdr *dns = (struct dns_hdr *)payload;
		dns->id = 0;

		uint16_t off = fill_ip_udp(m, cnode, plen,
			(uint16_t)(1024 + (fast_rand_next() & 0x7FFF)), 53);
		if (!off) {
			continue;
		}

		memcpy(rte_pktmbuf_mtod_offset(m, char *, off), payload, plen);
		tx_bytes += off + plen + sizeof(struct rte_ether_hdr);
	}
	return tx_bytes;
}

/* Mutator array -- registered in erroneous.h */
struct Mutator dns_mutators[] = {
	{ "tc",              mutation_dns_tc },
	{ "nxdomain",        mutation_dns_nxdomain },
	{ "qtype_invalid",   mutation_dns_qtype_inv },
	{ "label_overflow",  mutation_dns_label_overflow },
	{ "id_zero",         mutation_dns_id_zero },
};
