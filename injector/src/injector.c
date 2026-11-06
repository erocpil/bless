#include "injector.h"
#include "injector_parse.h"
#include "injector_plugin.h"
#include "log.h"

/**
 * @file injector.c
 * @brief Core packet construction engine and configuration plumbing.
 *
 * Provides:
 *   - Packet mbuf allocation / pool creation
 *   - Protocol-agnostic dispatch (injector_mbufs) with VXLAN encap
 *   - Distribution table construction (injector_set_dist)
 *   - CLI argument parsing (injector_parse_type)
 */

/* Logger prefix -- used by LOG_* macros */
#include "base.h"
#include "dist.h"

/* Set inner L3/L4 checksum metadata after an outer VXLAN header was
 * prepended.  Only IPv4 inner frames have IP checksum metadata. */
static void injector_set_inner_l4_offload(struct rte_mbuf *m,
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
 * Returns the number of bytes prepended (INJECTOR_SIZEOF_VXLAN)
 * or -1 on mbuf prepend failure. */
uint64_t injector_encap_vxlan(struct rte_mbuf **mbufs, unsigned int n, void *data)
{
	Cnode *cnode = (Cnode*)data;

	for (int i = 0; i < (int)n; i++) {
		struct rte_mbuf *m = mbufs[i];

		char *data = rte_pktmbuf_prepend(m, INJECTOR_SIZEOF_VXLAN);
		if (!data) {
			LOG_ERR("Cannot rte_pktmbuf_prepend(%p %lu) headroom=%u -- skipping",
				m, INJECTOR_SIZEOF_VXLAN, rte_pktmbuf_headroom(m));
			continue;
		}

		struct rte_ether_hdr *eth = (struct rte_ether_hdr *)data;
		struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
		struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip + 1);
		uint8_t *vxlan_hdr = (uint8_t *)(udp + 1);

		/* Outer L2 -- prefer VXLAN-specific MAC, fall back to inner MAC */
		if (cnode->vxlan.ether.n_src > 0)
			rte_ether_addr_copy((struct rte_ether_addr*)cnode->vxlan.ether.src,
					&eth->src_addr);
		else
			rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.src,
					&eth->src_addr);
		if (cnode->vxlan.ether.n_dst > 0)
			rte_ether_addr_copy((struct rte_ether_addr*)cnode->vxlan.ether.dst,
					&eth->dst_addr);
		else
			rte_ether_addr_copy((struct rte_ether_addr*)cnode->ether.dst,
					&eth->dst_addr);
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
		if (OFFLOAD_OUTER_UDP(cnode))
			m->ol_flags |= RTE_MBUF_F_TX_OUTER_UDP_CKSUM;

		injector_set_inner_l4_offload(m, INJECTOR_SIZEOF_VXLAN);

		m->outer_l2_len = sizeof(struct rte_ether_hdr);
		m->outer_l3_len = sizeof(struct rte_ipv4_hdr);
	}

	return INJECTOR_SIZEOF_VXLAN;
}

/* Packet construction entry-point: dispatch to the registered constructor.
 *
 * 1. Looks up a packet constructor by type_idx (built-in or extension).
 * 2. Calls the constructor to build inner-layer packets.
 * 3. Optionally wraps a subset (vxlan.ratio %) with VXLAN encapsulation.
 *
 * Returns total bytes constructed (inner + outer), or 0 on failure. */
uint64_t injector_mbufs(struct rte_mbuf **mbufs, uint32_t n, unsigned int type_idx,
		void *data)
{
	uint64_t r = 0;
	uint64_t tx_bytes  = 0;
	Cnode *cnode = (Cnode*)data;

	/* Look up constructor from registration -- works for both built-in
	 * (0-3) and extension (4+) type indices */
	injector_pkt_ctor_t ctor = injector_get_ctor(type_idx);
	if (!ctor) {
		LOG_ERR("type %u: no constructor registered", type_idx);
		return 0;
	}
	r = ctor(mbufs, n, cnode);
	if (!r) {
		LOG_ERR("injector_mbufs: constructor for type %u returned 0", type_idx);
		return 0;
	}
	if (cnode->vxlan.enable) {
		uint16_t ra = fast_rand_next() & 1023;
		if (cnode->vxlan.ratio > 0 && (ra % 100) < cnode->vxlan.ratio) {
			tx_bytes = injector_encap_vxlan(mbufs, n, cnode);
			if (!tx_bytes) {
				LOG_ERR("injector_mbufs: VXLAN encapsulator returned 0 for type %u",
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
int injector_alloc_mbufs(struct rte_mempool *pktmbuf_pool,
		struct rte_mbuf **mbufs, int n)
{
	if (rte_pktmbuf_alloc_bulk(pktmbuf_pool, mbufs, n) != 0) {
		LOG_ERR("%s %d: Cannot init mbuf(%d)", __func__, __LINE__, n);
		return -1;
	}
	return n;
}

void dist_ratio_init(struct dist_ratio *dr)
{
	dr->num = 1;
	for (int i = 0; i < TYPE_MAX; i++) {
		dr->weight[i] = 0;
		dr->quota[i] = 0;
	}
}

struct injector_conf *injector_init()
{
	struct injector_conf *bconf =
		(struct injector_conf*)malloc(sizeof(struct injector_conf));
	if (!bconf) {
		return NULL;
	}
	memset(bconf, 0, sizeof(struct injector_conf));
	bconf->batch = 64;
	bconf->num = -1;
	dist_ratio_init(&bconf->dist_ratio);

	return bconf;
}

/* Free all resources owned by a injector_conf.
 *
 * Releases malloc'd fields (bep) and rte_malloc'd fields (dist).
 *  */
void injector_free(struct injector_conf *bconf)
{
	if (!bconf) return;

	free(bconf->bep.inner);
	free(bconf->bep.outer);

	/* dist is rte_malloc'd so must use rte_free */
	rte_free(bconf->dist);

	free(bconf);
}

/* Log the internal state of a distribution table for debugging. */
void injector_show_dist(const struct distribution *dist)
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
 *      (built-in via ratio->weight[], extensions via injector_get_type_weight).
 *   2. First-pass distribute() allocates per-type packet counts (when
 *      bconf->num is finite) or uses weights directly (when num == -1,
 *      infinite mode).
 *   3. Second-pass distribute() fills the power-of-2 sized dispatch array
 *      so that fast_rand() & mask yields the correct distribution.
 *   4. Stores the inner/outer encap parameters in bconf->bep.
 *
 * On success the distribution is live in bconf->dist.
 * Returns 0 on success, exits on memory failure. */
int injector_set_dist(struct injector_conf* bconf, struct dist_ratio *ratio,
		struct injector_encap_params *bep)
{
	uint32_t n_types = 0;
	/* Collect type_idx + weight for all registered types with weight > 0.
	 * For backward compat, also read from ratio->weight[] for built-in types. */
	struct {
		unsigned int type_idx;
		int32_t      weight;
	} type_entry[INJECTOR_MAX_TYPES];

	/* First: iterate registration table for all types with weights */
	for (unsigned int i = 0; i < INJECTOR_MAX_TYPES; i++) {
		const char *name = injector_get_type_name(i);
		if (strcmp(name, "unknown") == 0)
			continue;

		int32_t w = injector_get_type_weight(name);

		/* For built-in types (0..TYPE_MAX-1), also read from legacy ratio weight */
		if (i < (unsigned int)TYPE_MAX && ratio->weight[i] > w)
			w = ratio->weight[i];

		if (w <= 0)
			continue;

		type_entry[n_types].type_idx = i;
		type_entry[n_types].weight   = w;
		n_types++;
	}

	uint64_t *result = NULL;

	LOG_META_NNL("weight from registration: ");
	for (unsigned int i = 0; i < n_types; i++) {
		printf("%s: %d, ", injector_get_type_name(type_entry[i].type_idx),
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

	/* Packet construction requires at least one non-zero protocol weight. */
	if (!n_types) {
		free(result);
		free(weights);
		bconf->dist = NULL;
		return -1;
	}

	/* num > 0: finite-packet mode — distribute packets proportionally.
	 * num == 0 or num == -1: unlimited — use raw weights as quotas. */
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
			printf("%s: %lu, ", injector_get_type_name(type_entry[i].type_idx),
			       result[i]);
		}
	} else {
		for (unsigned int i = 0; i < n_types; i++)
			result[i] = weights[i];
	}

	/* Update ratio->quota for backward compat (built-in types only) */
	memset(ratio->quota, 0, sizeof(ratio->quota[0]) * TYPE_MAX);
	for (unsigned int i = 0; i < n_types; i++) {
		if (type_entry[i].type_idx < (unsigned int)TYPE_MAX)
			ratio->quota[type_entry[i].type_idx] = (int32_t)result[i];
	}
	printf("\n");

	memcpy(&bconf->dist_ratio, ratio, sizeof(*ratio));

	uint32_t capacity = bconf->dist_ratio.num;
	if (capacity == 0) capacity = 1;
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
		printf("%s %lu, ", injector_get_type_name(type_entry[i].type_idx),
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

	injector_show_dist(bconf->dist);

	return 0;
}

/* Parse a per-type weight from a CLI argument string.
 *
 * Validates range [0, INT_MAX/TYPE_MAX) and bounds to avoid overflow.
 * Returns the parsed weight, INT_MIN for missing optarg,
 * or exits on invalid input. */
int32_t injector_parse_type(unsigned int type_idx, char *optarg)
{
	char *end = NULL;
	int64_t n = 0;

	if (type_idx >= INJECTOR_MAX_TYPES)
		goto ERROR;

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
	rte_exit(EXIT_FAILURE, "Invalid injector arguments `%s'\n", optarg);
}

void injector_print_mac(const struct rte_ether_addr *mac)
{
	char buf[RTE_ETHER_ADDR_FMT_SIZE];
	rte_ether_format_addr(buf, sizeof(buf), mac);
	printf("%s\n", buf);
}

void injector_print_ipv4(uint32_t ip)
{
	struct in_addr in;
	in.s_addr = ip;
	char buf[INET_ADDRSTRLEN];
	if (inet_ntop(AF_INET, &in, buf, sizeof(buf)) != NULL) {
		printf("IP: %s\n", buf);
	}
}
