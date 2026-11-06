#include "bless.h"
#include "bless_cfg.h"
#include "bless_plugin.h"
#include "config_node.h"
#include "dns.h"
#include <rte_udp.h>
#include <strings.h>   /* strcasecmp */

/**
 * @file proto_dns.c
 * @brief DNS query packet constructor (extension plugin).
 *
 * Builds DNS queries over UDP/53 with configurable domain names
 * and query types.  Registered as a bless extension via the plugin
 * system; type_idx and config fields are managed by bless_plugin.c.
 */

/* Extension config fields */
static const struct bless_cfg_field dns_cfg_fields[] = {
	{ "src",    BLESS_CFG_PORT_RANGE,  offsetof(struct dns_ext_cfg, src),
	  offsetof(struct dns_ext_cfg, n_src), PORT_MAX,
	  offsetof(struct dns_ext_cfg, src_range) },
	{ "dst",    BLESS_CFG_PORT_RANGE,  offsetof(struct dns_ext_cfg, dst),
	  offsetof(struct dns_ext_cfg, n_dst), PORT_MAX,
	  offsetof(struct dns_ext_cfg, dst_range) },
	{ NULL },
};

/* QTYPE name -> number mapping for YAML config */
static const struct {
	const char *name;
	uint16_t    value;
} qtype_map[] = {
	{ "A",     1  },
	{ "NS",    2  },
	{ "CNAME", 5  },
	{ "SOA",   6  },
	{ "MX",    15 },
	{ "TXT",   16 },
	{ "AAAA",  28 },
	{ "SRV",   33 },
	{ "ANY",   255 },
	{ NULL,    0  },
};

static uint16_t parse_qtype(const char *s)
{
	for (int i = 0; qtype_map[i].name; i++) {
		if (strcasecmp(s, qtype_map[i].name) == 0) {
			return qtype_map[i].value;
		}
	}
	/* numeric fallback */
	long v = strtol(s, NULL, 10);
	return (v > 0 && v <= 65535) ? (uint16_t)v : 1;  /* default: A */
}

/* DNS custom YAML parser -- handles dns.names and dns.qtypes arrays */
static int dns_parse_cfg_hook(void *node_v, void *cfg_v)
{
	Node *node = (Node *)node_v;
	struct dns_ext_cfg *cfg = (struct dns_ext_cfg *)cfg_v;

	/* Parse dns.names */
	Node *names_node = find_by_path(node, "names");
	if (names_node) {
		Node *n = names_node->child;
		cfg->n_names = 0;
		while (n && cfg->n_names < BLESS_CONFIG_MAX) {
			if (n->value) {
				const char *dot = n->value;
				char *wire = malloc(strlen(dot) + 2);
				if (!wire) {
					return -1;
				}
				char *wp = wire;
				const char *dp = dot;
				while (*dp) {
					const char *end = strchrnul(dp, '.');
					size_t len = (size_t)(end - dp);
					if (len > 63) { free(wire); return -1; }
					*wp++ = (char)len;
					memcpy(wp, dp, len);
					wp += len;
					dp = (*end == '.') ? end + 1 : end;
				}
				*wp++ = '\0';
				*wp = '\0';
				cfg->names[cfg->n_names++] = wire;
			}
			n = n->next;
		}
	}

	/* Parse dns.qtypes */
	Node *qtypes_node = find_by_path(node, "qtypes");
	if (qtypes_node) {
		Node *n = qtypes_node->child;
		cfg->n_qtypes = 0;
		while (n && cfg->n_qtypes < BLESS_CONFIG_MAX) {
			if (n->value) {
				cfg->qtypes[cfg->n_qtypes++] = parse_qtype(n->value);
			}
			n = n->next;
		}
	}

	/* Defaults if nothing configured */
	if (cfg->n_qtypes == 0) {
		cfg->qtypes[cfg->n_qtypes++] = 1; /* A */
	}

	return 0;
}

/* Config lifecycle */
static const char *default_names[] = {
	"www.example.com",
	"api.example.org",
	"cdn.example.net",
};
#define DEFAULT_N_NAMES 3

/* Config lifecycle */
static void dns_free_cfg(void *cfg_v)
{
	struct dns_ext_cfg *cfg = (struct dns_ext_cfg *)cfg_v;
	for (uint16_t i = 0; i < cfg->n_names; i++)
		free(cfg->names[i]);
}

static int dns_clone_cfg(const void *src_v, void *dst_v)
{
	const struct dns_ext_cfg *src = (const struct dns_ext_cfg *)src_v;
	struct dns_ext_cfg *dst = (struct dns_ext_cfg *)dst_v;
	for (uint16_t i = 0; i < src->n_names; i++)
		dst->names[i] = NULL;
	for (uint16_t i = 0; i < src->n_names; i++) {
		size_t len = strlen(src->names[i]) + 1;
		dst->names[i] = malloc(len);
		if (!dst->names[i]) {
			return -1;
		}
		memcpy(dst->names[i], src->names[i], len);
	}
	return 0;
}

static void dns_port_range(const void *cfg_v,
	int32_t *src_range, uint16_t *n_src,
	int32_t *dst_range, uint16_t *n_dst)
{
	const struct dns_ext_cfg *cfg = (const struct dns_ext_cfg *)cfg_v;
	*src_range = cfg->src_range;
	*n_src     = cfg->n_src;
	*dst_range = cfg->dst_range;
	*n_dst     = cfg->n_dst;
}

/* Random port + name + qtype selection */
static uint16_t rand_dns_src(struct dns_ext_cfg *cfg)
{
	if (cfg->src_range > 0) {
		return (uint16_t)(cfg->src[0] + (fast_rand_next() % cfg->src_range));
	}
	return cfg->n_src > 0
		? cfg->src[fast_rand_next() % cfg->n_src]
		: (uint16_t)(1024 + (fast_rand_next() & 0x7FFF));
}

static uint16_t rand_dns_dst(struct dns_ext_cfg *cfg)
{
	if (cfg->dst_range > 0) {
		return (uint16_t)(cfg->dst[0] + (fast_rand_next() % cfg->dst_range));
	}
	return cfg->n_dst > 0
		? cfg->dst[fast_rand_next() % cfg->n_dst]
		: 53;
}

static const char *rand_dns_name(struct dns_ext_cfg *cfg)
{
	uint16_t n = cfg->n_names;
	if (n == 0) {
		return default_names[fast_rand_next() % DEFAULT_N_NAMES];
	}
	return cfg->names[fast_rand_next() % n];
}

static uint16_t rand_dns_qtype(struct dns_ext_cfg *cfg)
{
	return cfg->qtypes[fast_rand_next() % cfg->n_qtypes];
}

/* Constructor */
uint64_t bless_mbufs_dns(struct rte_mbuf **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode *)data;
	struct dns_ext_cfg *cfg = dns_ext_find(cnode);

	const uint16_t l2_len = sizeof(struct rte_ether_hdr);
	const uint16_t l3_len = sizeof(struct rte_ipv4_hdr);
	const uint16_t l4_len = sizeof(struct rte_udp_hdr);
	const uint16_t dns_hdr  = DNS_HDR_SIZE;
	const uint16_t qtail    = DNS_QUESTION_TAIL;

	uint64_t tx_bytes = 0;

	for (int i = 0; i < (int)n; i++) {
		const char *wname = cfg ? rand_dns_name(cfg)
			: default_names[fast_rand_next() % DEFAULT_N_NAMES];
		uint16_t qname_len = (uint16_t)strlen(wname);  /* includes trailing \0 */
		uint16_t total_pkt_size = l2_len + l3_len + l4_len
			+ dns_hdr + qname_len + qtail;

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
		ip->total_length = rte_cpu_to_be_16(l3_len + l4_len
			+ dns_hdr + qname_len + qtail);
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
		udp->src_port = rte_cpu_to_be_16(cfg ? rand_dns_src(cfg)
			: (uint16_t)(1024 + (fast_rand_next() & 0x7FFF)));
		udp->dst_port = rte_cpu_to_be_16(cfg ? rand_dns_dst(cfg) : 53);
		udp->dgram_len = rte_cpu_to_be_16(l4_len + dns_hdr
			+ qname_len + qtail);
		udp->dgram_cksum = 0;  /* optional for IPv4 */

		struct dns_hdr *dns = (struct dns_hdr *)(udp + 1);
		memset(dns, 0, sizeof(*dns));
		dns->id      = rte_cpu_to_be_16((uint16_t)fast_rand_next());
		dns->flags   = rte_cpu_to_be_16(0x0100);  /* standard query, RD=1 */
		dns->qdcount = rte_cpu_to_be_16(1);

		/* QNAME (wire format, null-terminated list of labels) */
		memcpy((uint8_t *)dns + dns_hdr, wname, qname_len);

		/* QTYPE + QCLASS */
		struct dns_question_tail *qt =
			(struct dns_question_tail *)((uint8_t *)dns + dns_hdr + qname_len);
		uint16_t qtype = cfg ? rand_dns_qtype(cfg) : 1;
		qt->qtype  = rte_cpu_to_be_16(qtype);
		qt->qclass = rte_cpu_to_be_16(1);  /* IN */

		m->l2_len = l2_len;
		m->l3_len = l3_len;
		m->l4_len = l4_len;

		tx_bytes += total_pkt_size;
	}

	return tx_bytes;
}

/* Registration */
static const struct bless_pkt_type proto_dns = {
	.name       = "dns",
	.ether_type = RTE_ETHER_TYPE_IPV4,
	.ip_proto   = IPPROTO_UDP,  /* 17 -- DNS runs over UDP */
	.type_idx   = BLESS_AUTO_IDX,  /* auto-assigned extension type */
	.construct  = bless_mbufs_dns,
};

static const struct bless_ext_cfg dns_ext_cfg = {
	.name      = "dns",
	.cfg_size  = sizeof(struct dns_ext_cfg),
	.fields    = dns_cfg_fields,
	.yaml_path = "dns",
	.free_cfg  = dns_free_cfg,
	.clone_cfg = dns_clone_cfg,
	.port_range = dns_port_range,
	.parse_cfg  = dns_parse_cfg_hook,
};

static void __attribute__((constructor)) reg_dns(void)
{
	bless_register_pkt_type(&proto_dns);
	bless_register_cfg_parser(&dns_ext_cfg);
}
