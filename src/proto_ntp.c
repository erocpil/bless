#include "bless.h"
#include "bless_cfg.h"
#include "bless_plugin.h"
#include "ntp.h"
#include <rte_udp.h>

/**
 * @file proto_ntp.c
 * @brief NTP packet constructor (extension plugin).
 *
 * Builds NTP client/server packets over UDP/123 with configurable
 * modes and versions.  Four 64-bit timestamps are randomised for
 * maximum timing entropy.  Registered via the bless plugin system.
 */

/* Extension config fields */
static const struct bless_cfg_field ntp_cfg_fields[] = {
	{ "src", BLESS_CFG_PORT_RANGE, offsetof(struct ntp_ext_cfg, src),
	  offsetof(struct ntp_ext_cfg, n_src), PORT_MAX,
	  offsetof(struct ntp_ext_cfg, src_range) },
	{ "dst", BLESS_CFG_PORT_RANGE, offsetof(struct ntp_ext_cfg, dst),
	  offsetof(struct ntp_ext_cfg, n_dst), PORT_MAX,
	  offsetof(struct ntp_ext_cfg, dst_range) },
	{ NULL },
};

static void ntp_free_cfg(void *cfg_v) { (void)cfg_v; /* no heap fields */ }

static void ntp_port_range(const void *cfg_v,
	int32_t *src_range, uint16_t *n_src,
	int32_t *dst_range, uint16_t *n_dst)
{
	const struct ntp_ext_cfg *cfg = (const struct ntp_ext_cfg *)cfg_v;
	*src_range = cfg->src_range;
	*n_src     = cfg->n_src;
	*dst_range = cfg->dst_range;
	*n_dst     = cfg->n_dst;
}

/* Random port + mode + version selection */
static uint16_t rand_ntp_src(struct ntp_ext_cfg *cfg)
{
	if (cfg->src_range > 0) {
		return (uint16_t)(cfg->src[0] + (fast_rand_next() % cfg->src_range));
	}
	return cfg->n_src > 0
		? cfg->src[fast_rand_next() % cfg->n_src]
		: (uint16_t)(1024 + (fast_rand_next() & 0x7FFF));
}

static uint16_t rand_ntp_dst(struct ntp_ext_cfg *cfg)
{
	if (cfg->dst_range > 0) {
		return (uint16_t)(cfg->dst[0] + (fast_rand_next() % cfg->dst_range));
	}
	return cfg->n_dst > 0
		? cfg->dst[fast_rand_next() % cfg->n_dst]
		: 123;
}

/* Constructor */
uint64_t bless_mbufs_ntp(struct rte_mbuf **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode *)data;
	struct ntp_ext_cfg *cfg = ntp_ext_find(cnode);

	const uint16_t l2_len = sizeof(struct rte_ether_hdr);
	const uint16_t l3_len = sizeof(struct rte_ipv4_hdr);
	const uint16_t l4_len = sizeof(struct rte_udp_hdr);
	const uint16_t ntp_hdr = NTP_HDR_SIZE;
	const uint16_t total_pkt_size = l2_len + l3_len + l4_len + ntp_hdr;

	uint8_t modes[]    = { 3, 4 };             /* client, server */
	uint8_t n_modes    = 2;
	uint8_t versions[] = { 4 };
	uint8_t n_versions = 1;

	if (cfg) {
		if (cfg->n_modes > 0) {
			memcpy(modes, cfg->modes, cfg->n_modes);
			n_modes = cfg->n_modes;
		}
		if (cfg->n_versions > 0) {
			memcpy(versions, cfg->versions, cfg->n_versions);
			n_versions = cfg->n_versions;
		}
	}

	uint64_t tx_bytes = 0;

	for (int i = 0; i < (int)n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);

		if (!rte_pktmbuf_append(m, total_pkt_size)) {
			return 0;
		}

		struct rte_ether_hdr *eth =
			rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
		rte_ether_addr_copy((struct rte_ether_addr *)cnode->ether.src,
				    &eth->src_addr);
		rte_ether_addr_copy((struct rte_ether_addr *)cnode->ether.dst,
				    &eth->dst_addr);
		eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

		struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
		memset(ip, 0, sizeof(*ip));
		ip->version_ihl = (4u << 4) | (sizeof(struct rte_ipv4_hdr) >> 2);
		ip->total_length = rte_cpu_to_be_16(l3_len + l4_len + ntp_hdr);
		ip->packet_id = rte_cpu_to_be_16((uint16_t)fast_rand_next());
		ip->fragment_offset = rte_cpu_to_be_16(RTE_IPV4_HDR_DF_FLAG);
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

		struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip + 1);
		memset(udp, 0, sizeof(*udp));
		udp->src_port = rte_cpu_to_be_16(cfg ? rand_ntp_src(cfg)
			: (uint16_t)(1024 + (fast_rand_next() & 0x7FFF)));
		udp->dst_port = rte_cpu_to_be_16(cfg ? rand_ntp_dst(cfg) : 123);
		udp->dgram_len = rte_cpu_to_be_16(l4_len + ntp_hdr);
		udp->dgram_cksum = 0;

		struct ntp_hdr *ntp = (struct ntp_hdr *)(udp + 1);
		memset(ntp, 0, sizeof(*ntp));

		uint8_t mode  = modes[fast_rand_next() % n_modes];
		uint8_t ver   = versions[fast_rand_next() % n_versions];
		ntp->li_vn_mode = (0 << 6) | ((ver & 0x7) << 3) | (mode & 0x7);
		ntp->stratum     = (uint8_t)(fast_rand_next() & 0xF) + 1;  /* 1–16 */
		ntp->poll         = (int8_t)(fast_rand_next() & 0xF);
		ntp->precision    = (int8_t)-(fast_rand_next() & 0x1F);  /* -1 to -32 */

		/* Four timestamps -- full 64-bit random for timing entropy */
		uint64_t base = rte_rdtsc();
		ntp->ref_ts  = rte_cpu_to_be_64(base + (fast_rand_next() & 0xFFFFFFFFFFULL));
		ntp->orig_ts = rte_cpu_to_be_64(base + (fast_rand_next() & 0xFFFFFFFFFFULL));
		ntp->recv_ts = rte_cpu_to_be_64(base + (fast_rand_next() & 0xFFFFFFFFFFULL));
		ntp->xmit_ts = rte_cpu_to_be_64(base + (fast_rand_next() & 0xFFFFFFFFFFULL));

		m->l2_len = l2_len;
		m->l3_len = l3_len;
		m->l4_len = l4_len;

		tx_bytes += total_pkt_size;
	}

	return tx_bytes;
}

/* Registration */
static const struct bless_pkt_type proto_ntp = {
	.name       = "ntp",
	.ether_type = RTE_ETHER_TYPE_IPV4,
	.ip_proto   = IPPROTO_UDP,
	.type_idx   = BLESS_AUTO_IDX,
	.construct  = bless_mbufs_ntp,
};

static const struct bless_ext_cfg ntp_ext_cfg = {
	.name       = "ntp",
	.cfg_size   = sizeof(struct ntp_ext_cfg),
	.fields     = ntp_cfg_fields,
	.yaml_path  = "ntp",
	.free_cfg   = ntp_free_cfg,
	.port_range = ntp_port_range,
};

static void __attribute__((constructor)) reg_ntp(void)
{
	bless_register_pkt_type(&proto_ntp);
	bless_register_cfg_parser(&ntp_ext_cfg);
}
