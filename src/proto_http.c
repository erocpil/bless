#include "bless.h"
#include "bless_cfg.h"
#include "bless_plugin.h"
#include "http.h"
#include <rte_tcp.h>

/**
 * @file proto_http.c
 * @brief HTTP request constructor (extension plugin).
 *
 * Builds minimal HTTP/1.1 GET requests over TCP/80 with configurable
 * methods, URI paths, and Host headers.  Suitable for L7 DPI stress
 * testing of gateways, load balancers, and WAFs.
 *
 * Registered via the bless plugin system; type_idx auto-assigned.
 */

/* Extension config fields */
static const struct bless_cfg_field http_cfg_fields[] = {
	{ "src",     BLESS_CFG_PORT_RANGE, offsetof(struct http_ext_cfg, src),
	  offsetof(struct http_ext_cfg, n_src), PORT_MAX,
	  offsetof(struct http_ext_cfg, src_range) },
	{ "dst",     BLESS_CFG_PORT_RANGE, offsetof(struct http_ext_cfg, dst),
	  offsetof(struct http_ext_cfg, n_dst), PORT_MAX,
	  offsetof(struct http_ext_cfg, dst_range) },
	{ "methods", BLESS_CFG_STRING,     offsetof(struct http_ext_cfg, methods),
	  sizeof(((struct http_ext_cfg *)0)->methods), 0 },
	{ "paths",   BLESS_CFG_STRING,     offsetof(struct http_ext_cfg, paths),
	  sizeof(((struct http_ext_cfg *)0)->paths), 0 },
	{ "hosts",   BLESS_CFG_STRING,     offsetof(struct http_ext_cfg, hosts),
	  sizeof(((struct http_ext_cfg *)0)->hosts), 0 },
	{ NULL },
};

/* Defaults */
#define DEF_METHODS "GET"
#define DEF_PATHS   "/"
#define DEF_HOSTS   "example.com"

/* Simple comma-split + random pick (not hot-path -- called once per packet) */
static const char *rand_csv(const char *csv, const char *default_val)
{
	if (!csv || !csv[0]) {
		return default_val;
	}

	/* Count items */
	int n = 1;
	for (const char *p = csv; *p; p++)
		if (*p == ',') {
			n++;
		}

	int pick = fast_rand_next() % n;
	int cur = 0;
	const char *start = csv;
	const char *p;
	for (p = csv; *p; p++) {
		if (*p == ',') {
			if (cur == pick) {
				break;
			}
			cur++;
			start = p + 1;
		}
	}

	/* Return a static copy from a small ring of buffers (thread-safe:
	 * fast_rand_next is per-core, so each core gets its own slot). */
	static __thread char buf[256];
	size_t len = (size_t)(p - start);
	if (len > 255) {
		len = 255;
	}
	memcpy(buf, start, len);
	buf[len] = '\0';
	return buf;
}

/* TCP port selection */
static uint16_t rand_http_src(struct http_ext_cfg *cfg)
{
	if (cfg->src_range > 0) {
		return (uint16_t)(cfg->src[0] + (fast_rand_next() % cfg->src_range));
	}
	return cfg->n_src > 0
		? cfg->src[fast_rand_next() % cfg->n_src]
		: (uint16_t)(1024 + (fast_rand_next() & 0x7FFF));
}

static uint16_t rand_http_dst(struct http_ext_cfg *cfg)
{
	if (cfg->dst_range > 0) {
		return (uint16_t)(cfg->dst[0] + (fast_rand_next() % cfg->dst_range));
	}
	return cfg->n_dst > 0
		? cfg->dst[fast_rand_next() % cfg->n_dst]
		: 80;
}

/* Generate one HTTP GET request payload into a pre-sized buffer.
 * Returns the actual payload length (<= max_payload). */
static uint16_t build_http_payload(char *buf, uint16_t max_payload,
	struct http_ext_cfg *cfg)
{
	const char *method = DEF_METHODS;
	const char *path   = DEF_PATHS;
	const char *host   = DEF_HOSTS;

	if (cfg) {
		method = rand_csv(cfg->methods, DEF_METHODS);
		path   = rand_csv(cfg->paths,   DEF_PATHS);
		host   = rand_csv(cfg->hosts,   DEF_HOSTS);
	}

	int len = snprintf(buf, max_payload,
		"%s %s HTTP/1.1\r\n"
		"Host: %s\r\n"
		"User-Agent: bless/1.0\r\n"
		"Accept: */*\r\n"
		"Connection: close\r\n"
		"\r\n",
		method, path, host);

	return (len > 0 && (uint16_t)len < max_payload) ? (uint16_t)len : 0;
}

/* Config lifecycle */
static void http_free_cfg(void *cfg_v) { (void)cfg_v; }
static int http_clone_cfg(const void *src_v, void *dst_v)
{
	memcpy(dst_v, src_v, sizeof(struct http_ext_cfg));
	return 0;
}

static void http_port_range(const void *cfg_v,
	int32_t *src_range, uint16_t *n_src,
	int32_t *dst_range, uint16_t *n_dst)
{
	const struct http_ext_cfg *cfg = (const struct http_ext_cfg *)cfg_v;
	*src_range = cfg->src_range;
	*n_src     = cfg->n_src;
	*dst_range = cfg->dst_range;
	*n_dst     = cfg->n_dst;
}

/* Constructor */
uint64_t bless_mbufs_http(struct rte_mbuf **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode *)data;
	struct http_ext_cfg *cfg = http_ext_find(cnode);

	const uint16_t l2_len = sizeof(struct rte_ether_hdr);
	const uint16_t l3_len = sizeof(struct rte_ipv4_hdr);
	const uint16_t l4_len = sizeof(struct rte_tcp_hdr);

	/* Pre-generate a payload buffer -- consumes one fast_rand_next() call
	 * per packet but avoids snprintf in the hot loop.  For HTTP, the
	 * payload is variable-length and dominates packet size. */
	char payload_buf[512];
	uint16_t plen = build_http_payload(payload_buf, sizeof(payload_buf), cfg);
	if (!plen) {
		return 0;
	}

	uint16_t total_pkt_size = l2_len + l3_len + l4_len + plen;
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
		ip->total_length = rte_cpu_to_be_16(l3_len + l4_len + plen);
		ip->packet_id = rte_cpu_to_be_16((uint16_t)fast_rand_next());
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
		tcp->src_port = rte_cpu_to_be_16(cfg ? rand_http_src(cfg)
			: (uint16_t)(1024 + (fast_rand_next() & 0x7FFF)));
		tcp->dst_port = rte_cpu_to_be_16(cfg ? rand_http_dst(cfg) : 80);
		tcp->sent_seq = rte_cpu_to_be_32(fast_rand_next());
		tcp->recv_ack = 0;
		tcp->data_off = ((sizeof(struct rte_tcp_hdr) / 4) << 4) | RTE_TCP_PSH_FLAG | RTE_TCP_ACK_FLAG;
		tcp->tcp_flags = RTE_TCP_PSH_FLAG | RTE_TCP_ACK_FLAG;
		tcp->rx_win = rte_cpu_to_be_16(65535);
		tcp->cksum = 0;

		/* Copy HTTP payload */
		memcpy((uint8_t *)tcp + l4_len, payload_buf, plen);

		m->l2_len = l2_len;
		m->l3_len = l3_len;
		m->l4_len = l4_len;

		tx_bytes += total_pkt_size;
	}

	return tx_bytes;
}

/* Registration */
static const struct bless_pkt_type proto_http = {
	.name       = "http",
	.ether_type = RTE_ETHER_TYPE_IPV4,
	.ip_proto   = IPPROTO_TCP,
	.type_idx   = BLESS_AUTO_IDX,
	.construct  = bless_mbufs_http,
};

static const struct bless_ext_cfg http_ext_cfg = {
	.name       = "http",
	.cfg_size   = sizeof(struct http_ext_cfg),
	.fields     = http_cfg_fields,
	.yaml_path  = "http",
	.free_cfg   = http_free_cfg,
	.clone_cfg  = http_clone_cfg,
	.port_range = http_port_range,
};

static void __attribute__((constructor)) reg_http(void)
{
	bless_register_pkt_type(&proto_http);
	bless_register_cfg_parser(&http_ext_cfg);
}
