#include "bless.h"
#include "ntp.h"
#include <rte_udp.h>

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

/**
 * @file mutation_ntp.c
 * @brief NTP protocol mutations.
 *
 * Reconstructs NTP packets with deliberately mangled fields to
 * stress-test NTP protocol parsers and DPI session trackers.
 */

static uint16_t fill_ip_udp_ntp(struct rte_mbuf *m, Cnode *cnode,
	uint16_t src_port, uint16_t dst_port)
{
	const uint16_t l2_len = sizeof(struct rte_ether_hdr);
	const uint16_t l3_len = sizeof(struct rte_ipv4_hdr);
	const uint16_t l4_len = sizeof(struct rte_udp_hdr);
	const uint16_t pkt_len = l2_len + l3_len + l4_len + NTP_HDR_SIZE;

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
	ip->total_length = rte_cpu_to_be_16(l3_len + l4_len + NTP_HDR_SIZE);
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
	udp->dgram_len = rte_cpu_to_be_16(l4_len + NTP_HDR_SIZE);

	m->l2_len = l2_len;
	m->l3_len = l3_len;
	m->l4_len = l4_len;
	return l3_len + l4_len;
}

/* mutation_ntp_stratum -- stratum 0 (kiss-o'-death) or 16 (unsync) */
uint64_t mutation_ntp_stratum(void **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode *)data;
	uint64_t tx_bytes = 0;
	for (unsigned i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);
		uint16_t off = fill_ip_udp_ntp(m, cnode,
			(uint16_t)(1024 + (fast_rand_next() & 0x7FFF)), 123);
		if (!off) {
			continue;
		}

		struct ntp_hdr *ntp = rte_pktmbuf_mtod_offset(m, struct ntp_hdr *, off);
		uint8_t mode = 3 + (fast_rand_next() & 1); /* 3=client, 4=server */
		ntp->li_vn_mode = (0 << 6) | (4 << 3) | mode;
		ntp->stratum = (fast_rand_next() & 1) ? 0 : 16;

		ntp->xmit_ts = rte_cpu_to_be_64(rte_rdtsc());
		tx_bytes += off + NTP_HDR_SIZE + sizeof(struct rte_ether_hdr);
	}
	return tx_bytes;
}

/* mutation_ntp_li -- leap indicator = 3 (clock unsynchronized) */
uint64_t mutation_ntp_li(void **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode *)data;
	uint64_t tx_bytes = 0;
	for (unsigned i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);
		uint16_t off = fill_ip_udp_ntp(m, cnode,
			(uint16_t)(1024 + (fast_rand_next() & 0x7FFF)), 123);
		if (!off) {
			continue;
		}

		struct ntp_hdr *ntp = rte_pktmbuf_mtod_offset(m, struct ntp_hdr *, off);
		ntp->li_vn_mode = (3 << 6) | (4 << 3) | 3; /* LI=3, VN=4, mode=client */
		ntp->stratum = 1;
		ntp->xmit_ts = rte_cpu_to_be_64(rte_rdtsc());
		tx_bytes += off + NTP_HDR_SIZE + sizeof(struct rte_ether_hdr);
	}
	return tx_bytes;
}

/* mutation_ntp_version -- invalid version (0, 5, 6, 7) */
uint64_t mutation_ntp_version(void **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode *)data;
	uint64_t tx_bytes = 0;
	uint8_t bad_vers[] = {0, 5, 6, 7};
	for (unsigned i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);
		uint16_t off = fill_ip_udp_ntp(m, cnode,
			(uint16_t)(1024 + (fast_rand_next() & 0x7FFF)), 123);
		if (!off) {
			continue;
		}

		struct ntp_hdr *ntp = rte_pktmbuf_mtod_offset(m, struct ntp_hdr *, off);
		uint8_t ver = bad_vers[fast_rand_next() & 3];
		ntp->li_vn_mode = (0 << 6) | (ver << 3) | 3;
		ntp->stratum = 1;
		ntp->xmit_ts = rte_cpu_to_be_64(rte_rdtsc());
		tx_bytes += off + NTP_HDR_SIZE + sizeof(struct rte_ether_hdr);
	}
	return tx_bytes;
}

/* mutation_ntp_timestamp_zero -- zero all timestamps */
uint64_t mutation_ntp_timestamp_zero(void **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode *)data;
	uint64_t tx_bytes = 0;
	for (unsigned i = 0; i < n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);
		uint16_t off = fill_ip_udp_ntp(m, cnode,
			(uint16_t)(1024 + (fast_rand_next() & 0x7FFF)), 123);
		if (!off) {
			continue;
		}

		struct ntp_hdr *ntp = rte_pktmbuf_mtod_offset(m, struct ntp_hdr *, off);
		ntp->li_vn_mode = (0 << 6) | (4 << 3) | 3;
		ntp->stratum = 1;
		/* leave all timestamps at zero */
		tx_bytes += off + NTP_HDR_SIZE + sizeof(struct rte_ether_hdr);
	}
	return tx_bytes;
}

struct Mutator ntp_mutators[] = {
	{ "stratum",       mutation_ntp_stratum },
	{ "li",            mutation_ntp_li },
	{ "version",       mutation_ntp_version },
	{ "timestamp_zero",mutation_ntp_timestamp_zero },
};
