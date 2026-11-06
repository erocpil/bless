#include "bless.h"
#include "bless_cfg.h"
#include "bless_plugin.h"
#include "sctp.h"
#include <rte_sctp.h>

/**
 * @file proto_sctp.c
 * @brief SCTP packet constructor (extension plugin).
 *
 * Builds SCTP chunks with configurable src/dst ports, verification
 * tag, and payload.  Registered as a bless extension via the plugin
 * system; its type_idx and config fields are managed by bless_plugin.c.
 */

/* Extension config struct (stored in Cnode::ext[]) */
static const struct bless_cfg_field sctp_cfg_fields[] = {
	{ "src",    BLESS_CFG_PORT_RANGE,  offsetof(struct sctp_ext_cfg, src),
	  offsetof(struct sctp_ext_cfg, n_src), PORT_MAX,
	  offsetof(struct sctp_ext_cfg, src_range) },
	{ "dst",    BLESS_CFG_PORT_RANGE,  offsetof(struct sctp_ext_cfg, dst),
	  offsetof(struct sctp_ext_cfg, n_dst), PORT_MAX,
	  offsetof(struct sctp_ext_cfg, dst_range) },
	{ "payload", BLESS_CFG_STRING,     offsetof(struct sctp_ext_cfg, payload),
	  offsetof(struct sctp_ext_cfg, payload_len), 0 },
	{ NULL },
};

static void sctp_free_cfg(void *cfg_v)
{
	struct sctp_ext_cfg *cfg = (struct sctp_ext_cfg *)cfg_v;
	free(cfg->payload);
}

static int sctp_clone_cfg(const void *src_v, void *dst_v)
{
	const struct sctp_ext_cfg *src = (const struct sctp_ext_cfg *)src_v;
	struct sctp_ext_cfg *dst = (struct sctp_ext_cfg *)dst_v;
	dst->payload = NULL;
	if (src->payload && src->payload_len) {
		dst->payload = (char *)malloc(src->payload_len);
		if (!dst->payload) {
			return -1;
		}
		memcpy(dst->payload, src->payload, src->payload_len);
	}
	return 0;
}

static void sctp_port_range(const void *cfg_v,
	int32_t *src_range, uint16_t *n_src,
	int32_t *dst_range, uint16_t *n_dst)
{
	const struct sctp_ext_cfg *cfg = (const struct sctp_ext_cfg *)cfg_v;
	*src_range = cfg->src_range;
	*n_src = cfg->n_src;
	*dst_range = cfg->dst_range;
	*n_dst = cfg->n_dst;
}

/* Random port selection (matching RANDOM_TCP_SRC / RANDOM_UDP_SRC) */
static uint16_t rand_sctp_src(struct sctp_ext_cfg *cfg)
{
	if (cfg->src_range > 0) {
		return (uint16_t)(cfg->src[0] + (fast_rand_next() % cfg->src_range));
	}
	return cfg->n_src > 0
		? cfg->src[fast_rand_next() % cfg->n_src]
		: 0;
}

static uint16_t rand_sctp_dst(struct sctp_ext_cfg *cfg)
{
	if (cfg->dst_range > 0) {
		return (uint16_t)(cfg->dst[0] + (fast_rand_next() % cfg->dst_range));
	}
	return cfg->n_dst > 0
		? cfg->dst[fast_rand_next() % cfg->n_dst]
		: 0;
}

/* Constructor */
uint64_t bless_mbufs_sctp(struct rte_mbuf **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode *)data;
	struct sctp_ext_cfg *cfg = sctp_ext_find(cnode);
	if (!cfg) {
		return 0; /* no SCTP config -- nothing to do */
	}

	const uint16_t l2_len = sizeof(struct rte_ether_hdr);
	const uint16_t l3_len = sizeof(struct rte_ipv4_hdr);
	const uint16_t l4_len = sizeof(struct rte_sctp_hdr);
	uint16_t payload_len = cfg->payload_len;
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
			payload_len = payload_len < payload_len_fixed ? payload_len : payload_len_fixed;
		}
	}
	/* IMIX override */
	if (cnode->ether.n_imix) {
		uint16_t imix_pl = IMIX_PAYLOAD_LEN(cnode, l3_len, l4_len);
		if (imix_pl) {
			payload_len_fixed = imix_pl;
			payload_len = payload_len < payload_len_fixed ? payload_len : payload_len_fixed;
		}
	}
	const uint16_t total_pkt_size = l2_len + l3_len + l4_len + payload_len_fixed;

	uint64_t tx_bytes = 0;

	for (int i = 0; i < (int)n; i++) {
		struct rte_mbuf *m = mbufs[i];
		rte_pktmbuf_reset(m);

		if (!rte_pktmbuf_append(m, total_pkt_size)) {
			return 0;
		}

		struct rte_ether_hdr *eth_hdr =
			rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
		rte_ether_addr_copy((struct rte_ether_addr *)cnode->ether.src,
				    &eth_hdr->src_addr);
		rte_ether_addr_copy((struct rte_ether_addr *)cnode->ether.dst,
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
		ip->next_proto_id = IPPROTO_SCTP;  /* 132 */
		ip->src_addr = RANDOM_IP_SRC(cnode);
		ip->dst_addr = RANDOM_IP_DST(cnode);
		ip->hdr_checksum = 0;
		if (OFFLOAD_IPV4(cnode)) {
			m->ol_flags |= RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM;
		} else {
			ip->hdr_checksum = rte_ipv4_cksum(ip);
		}

		struct rte_sctp_hdr *sctp = (struct rte_sctp_hdr *)(ip + 1);
		memset(sctp, 0, sizeof(*sctp));
		sctp->src_port = rte_cpu_to_be_16(rand_sctp_src(cfg));
		sctp->dst_port = rte_cpu_to_be_16(rand_sctp_dst(cfg));
		sctp->tag      = rte_cpu_to_be_32(fast_rand_next());
		sctp->cksum    = 0;  /* 0 = skip verification, OK for load testing */
		if (OFFLOAD_SCTP(cnode)) {
			m->ol_flags |= RTE_MBUF_F_TX_SCTP_CKSUM;
		}

		if (cnode->ether.copy_payload && cfg->payload && payload_len) {
			rte_memcpy((uint8_t *)sctp + l4_len, cfg->payload, payload_len);
		}

		m->l2_len = l2_len;
		m->l3_len = l3_len;
		m->l4_len = l4_len;

		tx_bytes += total_pkt_size;
	}

	return tx_bytes;
}

/* Registration */
static const struct bless_pkt_type proto_sctp = {
	.name = "sctp",
	.ether_type = RTE_ETHER_TYPE_IPV4,
	.ip_proto = 132,
	.type_idx = TYPE_SCTP,
	.construct = bless_mbufs_sctp,
};

static const struct bless_ext_cfg sctp_ext_cfg = {
	.name      = "sctp",
	.cfg_size  = sizeof(struct sctp_ext_cfg),
	.fields    = sctp_cfg_fields,
	.yaml_path = "sctp",
	.free_cfg  = sctp_free_cfg,
	.clone_cfg = sctp_clone_cfg,
	.port_range = sctp_port_range,
};

static void __attribute__((constructor)) reg_sctp(void)
{
	bless_register_pkt_type(&proto_sctp);
	bless_register_cfg_parser(&sctp_ext_cfg);
}
