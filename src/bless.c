#include "bless.h"
#include "bless_parse.h"
#include "bless_plugin.h"
#include "log.h"

/**
 * @file bless.c
 * @brief Core packet construction engine and configuration plumbing.
 *
 * Provides:
 *   - Packet mbuf allocation / pool creation
 *   - Protocol-agnostic dispatch (bless_mbufs) with VXLAN encap
 *   - Distribution table construction (bless_set_dist)
 *   - CLI argument parsing (bless_parse_type)
 */

/* Logger prefix -- used by LOG_* macros */
#include "base.h"
#include "dist.h"

/* Set inner L3/L4 checksum metadata after an outer VXLAN header was
 * prepended.  The encapsulator must inspect the inner Ethernet type: an
 * IPv6 frame has a different header size and pseudo-header checksum. */
static void bless_set_inner_l4_offload(struct rte_mbuf *m,
		uint16_t inner_offset)
{
	struct rte_ether_hdr *inner_eth = rte_pktmbuf_mtod_offset(m,
			struct rte_ether_hdr *, inner_offset);
	uint16_t ether_type = rte_be_to_cpu_16(inner_eth->ether_type);

	if (ether_type == RTE_ETHER_TYPE_IPV4) {
		struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(inner_eth + 1);
		if (ip->next_proto_id == IPPROTO_UDP) {
			struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip + 1);
			m->ol_flags |= RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM |
				RTE_MBUF_F_TX_UDP_CKSUM;
			udp->dgram_cksum = rte_ipv4_phdr_cksum(ip, m->ol_flags);
		} else if (ip->next_proto_id == IPPROTO_TCP) {
			struct rte_tcp_hdr *tcp = (struct rte_tcp_hdr *)(ip + 1);
			m->ol_flags |= RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_IP_CKSUM |
				RTE_MBUF_F_TX_TCP_CKSUM;
			tcp->cksum = rte_ipv4_phdr_cksum(ip, m->ol_flags);
		}
	} else if (ether_type == RTE_ETHER_TYPE_IPV6) {
		struct rte_ipv6_hdr *ip6 = (struct rte_ipv6_hdr *)(inner_eth + 1);
		if (ip6->proto == IPPROTO_UDP) {
			struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip6 + 1);
			m->ol_flags |= RTE_MBUF_F_TX_IPV6 | RTE_MBUF_F_TX_UDP_CKSUM;
			udp->dgram_cksum = rte_ipv6_phdr_cksum(ip6, m->ol_flags);
		} else if (ip6->proto == IPPROTO_TCP) {
			struct rte_tcp_hdr *tcp = (struct rte_tcp_hdr *)(ip6 + 1);
			m->ol_flags |= RTE_MBUF_F_TX_IPV6 | RTE_MBUF_F_TX_TCP_CKSUM;
			tcp->cksum = rte_ipv6_phdr_cksum(ip6, m->ol_flags);
		}
	}
}

/** Prepend a VXLAN tunnel header to each mbuf in the burst.
 *
 * Stacks: Outer Eth -> Outer IPv4 -> Outer UDP -> VXLAN (8 bytes, VNI from
 * RANDOM_VXLAN_IP_VNI) -> original inner frame.  Outer L2 MAC falls back
 * to the inner MAC when no VXLAN-specific MAC is configured.
 *
 * HW offload flags are set per mbuf: tunnel / outer IP&UDP checksum,
 * plus inner L4 checksum conditional on the inner protocol (UDP=>UDP_CKSUM,
 * TCP=>TCP_CKSUM, else none).
 *
 * Returns the number of bytes prepended (BLESS_SIZEOF_VXLAN)
 * or -1 on mbuf prepend failure. */
uint64_t bless_encap_vxlan(struct rte_mbuf **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode*)data;

	for (int i = 0; i < (int)n; i++) {
		struct rte_mbuf *m = mbufs[i];

		char *data = rte_pktmbuf_prepend(m, BLESS_SIZEOF_VXLAN);
		if (!data) {
			LOG_ERR("Cannot rte_pktmbuf_prepend(%p %lu) headroom=%u -- skipping",
				m, BLESS_SIZEOF_VXLAN, rte_pktmbuf_headroom(m));
			continue;
		}

		struct rte_ether_hdr *eth = (struct rte_ether_hdr *)data;
		struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
		struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip + 1);
		uint8_t *vxlan_hdr = (uint8_t *)(udp + 1);

		/* Outer L2 -- prefer VXLAN-specific MAC, fall back to inner MAC */
		if (cnode->vxlan.ether.n_src > 0) {
			rte_ether_addr_copy((struct rte_ether_addr*)cnode->vxlan.ether.src,
					&eth->src_addr);
		} else {
			rte_ether_addr_copy((struct rte_ether_addr *)cnode->ether.src,
					    &eth->src_addr);
		}
		if (cnode->vxlan.ether.n_dst > 0) {
			rte_ether_addr_copy((struct rte_ether_addr*)cnode->vxlan.ether.dst,
					&eth->dst_addr);
		} else {
			rte_ether_addr_copy((struct rte_ether_addr *)cnode->ether.dst,
					    &eth->dst_addr);
		}
		eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

		/* Outer L3 */
		ip->version_ihl = 0x45;
		ip->type_of_service = 0;
		ip->total_length = rte_cpu_to_be_16(m->pkt_len -
				sizeof(struct rte_ether_hdr));
		ip->packet_id = 0;
		ip->fragment_offset = 0;
		ip->time_to_live = 64;
		ip->next_proto_id = IPPROTO_UDP;
		uint64_t addr_vni = RANDOM_VXLAN_IP_VNI(cnode);
		ip->src_addr = (uint32_t)addr_vni;
		ip->dst_addr = RANDOM_VXLAN_IP_DST(cnode);
		ip->hdr_checksum = 0;

		/* Outer UDP */
		udp->src_port = RANDOM_VXLAN_UDP_SRC(cnode);
		udp->dst_port = rte_cpu_to_be_16(RTE_VXLAN_DEFAULT_PORT);
		udp->dgram_len = rte_cpu_to_be_16(m->pkt_len -
				sizeof(struct rte_ether_hdr) - sizeof(struct rte_ipv4_hdr));
		udp->dgram_cksum = 0;

		/* VXLAN header */
		vxlan_hdr[0] = 0x08;
		vxlan_hdr[1] = 0;
		vxlan_hdr[2] = 0;
		vxlan_hdr[3] = 0;
		uint32_t vni = addr_vni >> 32;
		vxlan_hdr[4] = (vni >> 16) & 0xFF;
		vxlan_hdr[5] = (vni >> 8) & 0xFF;
		vxlan_hdr[6] = (vni) & 0xFF;
		vxlan_hdr[7] = 0;

		m->ol_flags = RTE_MBUF_F_TX_TUNNEL_VXLAN |
			RTE_MBUF_F_TX_OUTER_IPV4;

		if (OFFLOAD_OUTER_IPV4(cnode)) {
			m->ol_flags |= RTE_MBUF_F_TX_OUTER_IP_CKSUM;
		} else {
			ip->hdr_checksum = rte_ipv4_cksum(ip);
		}
		if (OFFLOAD_OUTER_UDP(cnode)) {
			m->ol_flags |= RTE_MBUF_F_TX_OUTER_UDP_CKSUM;
		}

		bless_set_inner_l4_offload(m, BLESS_SIZEOF_VXLAN);

		m->outer_l2_len = sizeof(struct rte_ether_hdr);
		m->outer_l3_len = sizeof(struct rte_ipv4_hdr);
	}

	return BLESS_SIZEOF_VXLAN;
}

/* Prepend a VXLAN-over-IPv6 tunnel header to each mbuf in the burst.
 *
 * Same as bless_encap_vxlan but uses IPv6 outer header:
 * Outer Eth -> Outer IPv6 -> Outer UDP -> VXLAN -> inner frame.
 *
 * Returns the number of bytes prepended (BLESS_SIZEOF_VXLAN6)
 * or -1 on mbuf prepend failure. */
static uint64_t bless_encap_vxlan6(struct rte_mbuf **mbufs, unsigned int n,
		void *data)
{
	Cnode *cnode = (Cnode*)data;

	for (int i = 0; i < (int)n; i++) {
		struct rte_mbuf *m = mbufs[i];

		char *d = rte_pktmbuf_prepend(m, BLESS_SIZEOF_VXLAN6);
		if (!d) {
			LOG_ERR("Cannot rte_pktmbuf_prepend(%p %lu) headroom=%u -- skipping",
				m, BLESS_SIZEOF_VXLAN6, rte_pktmbuf_headroom(m));
			continue;
		}

		struct rte_ether_hdr *eth = (struct rte_ether_hdr *)d;
		struct rte_ipv6_hdr *ip6 = (struct rte_ipv6_hdr *)(eth + 1);
		struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip6 + 1);
		uint8_t *vxlan_hdr = (uint8_t *)(udp + 1);

		/* Outer L2 -- same MAC fallback as IPv4 path */
		if (cnode->vxlan.ether.n_src > 0) {
			rte_ether_addr_copy((struct rte_ether_addr*)cnode->vxlan.ether.src,
					&eth->src_addr);
		} else {
			rte_ether_addr_copy((struct rte_ether_addr *)cnode->ether.src,
					    &eth->src_addr);
		}
		if (cnode->vxlan.ether.n_dst > 0) {
			rte_ether_addr_copy((struct rte_ether_addr*)cnode->vxlan.ether.dst,
					&eth->dst_addr);
		} else {
			rte_ether_addr_copy((struct rte_ether_addr *)cnode->ether.dst,
					    &eth->dst_addr);
		}
		eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV6);

		/* Outer IPv6 */
		memset(ip6, 0, sizeof(*ip6));
		ip6->vtc_flow = rte_cpu_to_be_32(0x60000000);
		ip6->payload_len = rte_cpu_to_be_16(m->pkt_len -
				sizeof(struct rte_ether_hdr) - sizeof(struct rte_ipv6_hdr));
		ip6->proto = IPPROTO_UDP;
		ip6->hop_limits = 64;
		random_array_elem_ipv6(ip6->src_addr,
			cnode->vxlan.ether.type.ipv6.src,
			cnode->vxlan.ether.type.ipv6.n_src,
			cnode->vxlan.ether.type.ipv6.src_range);
		random_array_elem_ipv6(ip6->dst_addr,
			cnode->vxlan.ether.type.ipv6.dst,
			cnode->vxlan.ether.type.ipv6.n_dst,
			cnode->vxlan.ether.type.ipv6.dst_range);

		/* Outer UDP */
		udp->src_port = RANDOM_VXLAN6_UDP_SRC(cnode);
		udp->dst_port = rte_cpu_to_be_16(RTE_VXLAN_DEFAULT_PORT);
		udp->dgram_len = rte_cpu_to_be_16(m->pkt_len -
				sizeof(struct rte_ether_hdr) - sizeof(struct rte_ipv6_hdr));
		udp->dgram_cksum = 0;

		/* VXLAN header (same as IPv4 path -- VNI is IP-version independent) */
		vxlan_hdr[0] = 0x08;
		vxlan_hdr[1] = 0;
		vxlan_hdr[2] = 0;
		vxlan_hdr[3] = 0;
		uint32_t vni = fast_rand_next() & 0xFFFFFF;
		vxlan_hdr[4] = (vni >> 16) & 0xFF;
		vxlan_hdr[5] = (vni >> 8) & 0xFF;
		vxlan_hdr[6] = (vni) & 0xFF;
		vxlan_hdr[7] = 0;

		m->ol_flags = RTE_MBUF_F_TX_TUNNEL_VXLAN |
			RTE_MBUF_F_TX_OUTER_IPV6 |
			RTE_MBUF_F_TX_IPV6;

		if (OFFLOAD_OUTER_UDP(cnode)) {
			m->ol_flags |= RTE_MBUF_F_TX_OUTER_UDP_CKSUM;
		}

		bless_set_inner_l4_offload(m, BLESS_SIZEOF_VXLAN6);

		m->outer_l2_len = sizeof(struct rte_ether_hdr);
		m->outer_l3_len = sizeof(struct rte_ipv6_hdr);
	}

	return BLESS_SIZEOF_VXLAN6;
}

static uint64_t (*bless_encap_outer[])(struct rte_mbuf **mbufs,
		unsigned int n, void *data) = {
	bless_encap_vxlan,
	bless_encap_vxlan6,
	NULL
};

/* Packet construction entry-point: dispatch to the registered constructor.
 *
 * 1. Looks up a packet constructor by type_idx (built-in or extension).
 * 2. Calls the constructor to build inner-layer packets.
 * 3. Optionally wraps a subset (vxlan.ratio %) with VXLAN encapsulation.
 *
 * Returns total bytes constructed (inner + outer), or 0 on failure. */
uint64_t bless_mbufs(struct rte_mbuf **mbufs, uint32_t n, unsigned int type_idx,
		void *data)
{
	uint64_t r = 0;
	uint64_t tx_bytes  = 0;
	Cnode *cnode = (Cnode*)data;

	/* Look up constructor from registration -- works for both built-in
	 * (0-4) and extension (5+) type indices */
	bless_pkt_ctor_t ctor = bless_get_ctor(type_idx);
	if (!ctor) {
		LOG_ERR("type %u: no constructor registered", type_idx);
		return 0;
	}
	r = ctor(mbufs, n, cnode);
	if (!r) {
		LOG_ERR("bless_mbufs: constructor for type %u returned 0", type_idx);
		return 0;
	}
	if (cnode->vxlan.enable) {
		uint16_t ra = fast_rand_next() & 1023;
		if (cnode->vxlan.ratio > 0 && (ra % 100) < cnode->vxlan.ratio) {
			tx_bytes = bless_encap_outer[cnode->vxlan.outer_ipv6 ? 1 : 0](mbufs, n, cnode);
			if (!tx_bytes) {
				LOG_ERR("bless_mbufs: VXLAN encapsulator returned 0 for type %u",
				        type_idx);
				return 0;
			}
		}
	}

	return tx_bytes + r;
}

/* Bulk-allocate ``n`` mbufs from a packet mempool.
 *
 * Calls rte_pktmbuf_alloc_bulk() which is faster than individual allocs.
 * Exits on failure -- DPDK cannot recover from pool exhaustion at TX time.
 * Returns n on success, -1 on failure. */
int bless_alloc_mbufs(struct rte_mempool *pktmbuf_pool,
		struct rte_mbuf **mbufs, int n)
{
	if (rte_pktmbuf_alloc_bulk(pktmbuf_pool, mbufs, n) != 0) {
		LOG_ERR("%s %d: Cannot init mbuf(%d)", __func__, __LINE__, n);
		return -1;
	}
	return n;
}

/* Create a DPDK mempool for packet mbufs.
 *
 * ``n`` is the pool capacity (capped below INT_MAX/2 for safety).
 * ``name`` is the DPDK pool name (must be unique per process).
 * Returns the pool pointer or exits on failure. */
struct rte_mempool *bless_create_pktmbuf_pool(uint32_t n, char *name)
{
	if (n >= (INT_MAX >> 1)) {
		LOG_ERR("Too many mbuf %u", n);
		return NULL;
	}

	LOG_TRACE("creating pktmbufpool %s %u", name, n);
	struct rte_mempool *pktmbuf_pool = rte_pktmbuf_pool_create(name, n,
			0 /* MEMPOOL_CACHE_SIZE */, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
			rte_socket_id());
	if (!pktmbuf_pool) {
		LOG_ERR("Cannot init rte_pktmbuf_pool_create()");
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

struct bless_conf *bless_init()
{
	struct bless_conf *bconf =
		(struct bless_conf*)malloc(sizeof(struct bless_conf));
	if (!bconf) {
		return NULL;
	}
	memset(bconf, 0, sizeof(struct bless_conf));
	bconf->preflight_mode = 1;
	bconf->environment_dump_path = strdup("bless-environment.json");
	dist_ratio_init(&bconf->dist_ratio);
	if (runtime_control_init(&bconf->runtime) != 0) {
		free(bconf);
		return NULL;
	}

	return bconf;
}

/* Free all resources owned by a bless_conf.
 *
 * Releases malloc'd fields (bep) and rte_malloc'd fields (dist).
 * Does NOT free stats (owned by caller/main.c quit path). */
void bless_free(struct bless_conf *bconf)
{
	if (!bconf) {
		return;
	}

	free(bconf->bep.inner);
	free(bconf->bep.outer);
	free(bconf->stats_dump_path);
	free(bconf->environment_dump_path);

	/* dist is rte_malloc'd so must use rte_free */
	rte_free(bconf->dist);

	runtime_control_destroy(&bconf->runtime);
	free(bconf);
}

void bless_sync_atomic_runtime(struct bless_conf *bconf)
{
	if (!bconf) {
		return;
	}
	runtime_control_publish_pps_rate(&bconf->runtime, bconf->pps_rate);
	runtime_control_publish_entropy_target(&bconf->runtime,
					       bconf->entropy_target);
	runtime_control_publish_entropy_dim(&bconf->runtime,
					    bconf->entropy_dim);
	runtime_control_publish_entropy_adapt_gain(
		&bconf->runtime, bconf->entropy_adapt_gain);
}

/* Log the internal state of a distribution table for debugging. */
void bless_show_dist(const struct distribution *dist)
{
	if (!dist) {
		return;
	}

	LOG_HINT("distribution  %p", dist);
	LOG_SHOW("  pos        %u", dist->pos);
	LOG_SHOW("  size       %u", dist->size);
	LOG_SHOW("  mask       %u", dist->mask);
	LOG_SHOW("  capacity   %u", dist->capacity);
}


/* Build the O(1) random-type dispatch table from configured weights.
 *
 * Flow:
 *   1. Collect all registered type-indices with positive weight
 *      (built-in via ratio->weight[], extensions via bless_get_type_weight).
 *   2. First-pass distribute() allocates per-type packet counts (when
 *      bconf->num is finite) or uses weights directly (when num == -1,
 *      infinite mode).
 *   3. Second-pass distribute() fills the power-of-2 sized dispatch array
 *      so that fast_rand() & mask yields the correct distribution.
 *   4. Stores the inner/outer encap parameters in bconf->bep.
 *
 * On success the distribution is live in bconf->dist.
 * Returns 0 on success, exits on memory failure. */
int bless_set_dist(struct bless_conf* bconf, struct dist_ratio *ratio,
		struct bless_encap_params *bep)
{
	uint32_t n_types = 0;
	/* Collect type_idx + weight for all registered types with weight > 0.
	 * For backward compat, also read from ratio->weight[] for built-in types. */
	struct {
		unsigned int type_idx;
		int32_t      weight;
	} type_entry[BLESS_MAX_TYPES];

	/* First: iterate registration table for all types with weights */
	for (unsigned int i = 0; i < BLESS_MAX_TYPES; i++) {
		const char *name = bless_get_type_name(i);
		if (strcmp(name, "unknown") == 0) {
			continue;
		}

		int32_t w = bless_get_type_weight(name);

		/* For built-in types (0..TYPE_MAX-1), also read from legacy ratio weight */
		if (i < (unsigned int)TYPE_MAX && ratio->weight[i] > w) {
			w = ratio->weight[i];
		}

		if (w <= 0) {
			continue;
		}

		type_entry[n_types].type_idx = i;
		type_entry[n_types].weight   = w;
		n_types++;
	}

	uint64_t *result = NULL;

	LOG_META_NNL("weight from registration: ");
	for (unsigned int i = 0; i < n_types; i++) {
		printf("%s: %d, ", bless_get_type_name(type_entry[i].type_idx),
		       type_entry[i].weight);
	}
	printf("\n");

	result = (uint64_t*)malloc(sizeof(uint64_t) * n_types);
	if (!result) {
		LOG_ERR("Cannot malloc(distribution result)");
		return -1;
	}

	/* Build weights array for distribute() */
	uint32_t *weights = (uint32_t*)malloc(sizeof(uint32_t) * n_types);
	if (!weights) {
		LOG_ERR("Cannot malloc(distribution weights)");
		free(result);
		return -1;
	}
	for (unsigned int i = 0; i < n_types; i++)
		weights[i] = (uint32_t)type_entry[i].weight;

	/* No non-zero weights -- skip distribution (pure handshake mode, etc.) */
	if (!n_types) {
		free(result);
		free(weights);
		bconf->dist = NULL;
		return 0;
	}

	/* num > 0: finite-packet mode — distribute packets proportionally.
	 * num < 0 (i.e. -1): unlimited — use raw weights as quotas.
	 * num == 0: zero-packet mode (no distribution needed). */
	int rc;
	if (bconf->num > 0) {
		rc = distribute(weights, n_types, (uint64_t)bconf->num, result);
		if (rc != 0) {
			LOG_ERR("distribute failed: %d (n_types=%u, num=%ld)",
				rc, n_types, (long)bconf->num);
			free(result);
			free(weights);
			return -1;
		}
		LOG_META_NNL("weight distribution: ");
		for (unsigned int i = 0; i < n_types; i++) {
			printf("%s: %lu, ", bless_get_type_name(type_entry[i].type_idx),
			       result[i]);
		}
	} else {
		for (unsigned int i = 0; i < n_types; i++)
			result[i] = weights[i];
	}

	/* Update ratio->quota for backward compat (built-in types only) */
	memset(ratio->quota, 0, sizeof(ratio->quota[0]) * TYPE_MAX);
	for (unsigned int i = 0; i < n_types; i++) {
		if (type_entry[i].type_idx < (unsigned int)TYPE_MAX) {
			ratio->quota[type_entry[i].type_idx] = (int32_t)result[i];
		}
	}
	printf("\n");

	memcpy(&bconf->dist_ratio, ratio, sizeof(*ratio));

	uint32_t capacity = bconf->dist_ratio.num;
	uint32_t size = 0;
	if (capacity > (1 << 16)) {
		size = 1 << 16;
		capacity = size;
	} else {
		size = make_power_of_2(capacity);
		if (size == 0 && capacity > 0) {
			LOG_ERR("make_power_of_2 overflow (capacity=%u)", capacity);
			free(result);
			free(weights);
			return -1;
		}
	}
	struct distribution *dist =
		rte_malloc(NULL, sizeof(struct distribution) +
				sizeof(uint8_t) * size, 0);
	if (!dist) {
		LOG_ERR("Cannot rte_malloc(distribution)");
		free(result);
		free(weights);
		return -1;
	}
	dist->size = size;

	/* Re-distribute to distribution table */
	rc = distribute(weights, n_types, size, result);
	if (rc != 0) {
		LOG_ERR("distribute (2nd pass) failed: %d (n_types=%u, size=%u)",
			rc, n_types, size);
		rte_free(dist);
		free(result);
		free(weights);
		return -1;
	}
	LOG_META_NNL("unified weight distribution: ");
	for (unsigned int i = 0; i < n_types; i++) {
		printf("%s %lu, ", bless_get_type_name(type_entry[i].type_idx),
		       result[i]);
	}
	printf("=> size %u\n", size);

	for (unsigned int i = 0, pos = 0, q = 0; i < n_types; i++) {
		for (uint32_t j = 0; j < result[q]; j++) {
			dist->data[pos++] = (uint8_t)type_entry[i].type_idx;
		}
		q++;
	}

	free(result);
	free(weights);

	dist->capacity = capacity;
	dist->mask = size - 1;
	bconf->dist = dist;

	bconf->bep.inner = bep->inner;
	bconf->bep.outer = bep->outer;

	bless_show_dist(bconf->dist);

	bless_sync_atomic_runtime(bconf);
	return 0;
}

/* Parse a per-type weight from a CLI argument string.
 *
 * Validates range [0, INT_MAX/TYPE_MAX) and bounds to avoid overflow.
 * Returns the parsed weight, INT_MIN for missing optarg,
 * or exits on invalid input. */
int32_t bless_parse_type(unsigned int type_idx, char *optarg)
{
	char *end = NULL;
	int64_t n = 0;

	if (type_idx >= BLESS_MAX_TYPES) {
		goto ERROR;
	}

	if (!optarg) {
		return INT_MIN;
	} else if ('-' == optarg[0]) {
		goto ERROR;
	}

	n = strtoul(optarg, &end, 0);
	if (n <= INT_MIN / 2 || n >= INT_MAX / 2) {
		goto ERROR;
	}
	if ('\0' == optarg[0] || !end || (*end != '\0')) {
		n = 0;
	} else if (n > INT_MAX / TYPE_MAX) {
		n = INT_MAX / TYPE_MAX;
	}

	return n;

ERROR:
	rte_exit(EXIT_FAILURE, "Invalid bless arguments `%s'\n", optarg);
}

void bless_print_mac(const struct rte_ether_addr *mac)
{
	char buf[RTE_ETHER_ADDR_FMT_SIZE];
	rte_ether_format_addr(buf, sizeof(buf), mac);
	printf("%s\n", buf);
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
