/**
 * @file entropy_stats.c
 * @brief Offline entropy computation engine.
 *
 * Drains per-worker entropy_sampler ring buffers and computes:
 *  - Shannon entropy H(X) and min-entropy H_inf(X) per dimension
 *  - Joint 5-tuple entropy
 *  - Pairwise mutual information I(X;Y) across 12 dimension pairs
 *  - Config-derived theoretical maximum per dimension
 *  - Latency histogram aggregation
 *  - EMA-diagonal smoothing of entropy time series
 *
 * Pure number-crunching: no dependency on the worker data plane
 * beyond reading the sampler ring buffers via entropy_samplers[].
 */

#include "bless.h"           /* LOG_*, rte_*, <sys/param.h> for min() */
#include "cnode.h"           /* Cnode */
#include "config.h"          /* dist_ratio */
#include "entropy.h"         /* entropy_sampler, shannon_from_sorted, cmp_u*, latency_hist */
#include "entropy_sampler_policy.h"
#include "entropy_stats.h"   /* own header */
#include "bless_plugin.h"    /* bless_ext_aggregate_port_ranges */
#include "server.h"          /* stats_snapshot */

#include <stdlib.h>          /* qsort, malloc, free */
#include <math.h>            /* log2, fmin */
#include <string.h>          /* memset */

static double
weighted_target(const int32_t *weights, size_t n, int *available)
{
	uint64_t total = 0, largest = 0;
	for (size_t i = 0; i < n; i++) {
		if (weights[i] <= 0) {
			continue;
		}
		total += (uint32_t)weights[i];
		if ((uint32_t)weights[i] > largest) {
			largest = (uint32_t)weights[i];
		}
	}
	*available = total > 0;
	return total ? -log2((double)largest / (double)total) : 0.0;
}

static unsigned
positive_weights(const int32_t *weights, size_t count)
{
	unsigned active = 0;
	for (size_t i = 0; i < count; i++) {
		if (weights[i] <= 0) {
			continue;
		}
		active++;
	}
	return active;
}

static double
conditional_pool_target(const int32_t *weights, size_t count,
			uint32_t pool_size, uint32_t excluded_types, int *available)
{
	uint64_t total = 0, excluded = 0;
	for (size_t i = 0; i < count; i++) {
		if (weights[i] <= 0) {
			continue;
		}
		total += (uint32_t)weights[i];
		if (i < 32 && (excluded_types & (1u << i))) {
			excluded += (uint32_t)weights[i];
		}
	}
	*available = total > 0 && pool_size > 0;
	if (!*available) {
		return 0.0;
	}
	double p_excluded = (double)excluded / (double)total;
	double p_pool_value = (double)(total - excluded) /
		((double)total * (double)pool_size);
	return -log2(fmax(p_excluded, p_pool_value));
}

static int32_t
type_weight(const struct dist_ratio *dr, unsigned type_idx)
{
	int32_t weight = bless_get_type_weight(bless_get_type_name(type_idx));
	if (type_idx < TYPE_MAX && dr->weight[type_idx] > weight) {
		weight = dr->weight[type_idx];
	}
	return weight > 0 ? weight : 0;
}

static uint32_t
configured_port_pool(const Cnode *cnode, unsigned type_idx, int source)
{
	if (type_idx == TYPE_TCP) {
		const typeof(cnode->ether.type.ipv4.tcp) *cfg =
			&cnode->ether.type.ipv4.tcp;
		return source ? (cfg->src_range > 0 ? (uint32_t)cfg->src_range : cfg->n_src)
			: (cfg->dst_range > 0 ? (uint32_t)cfg->dst_range : cfg->n_dst);
	}
	if (type_idx == TYPE_UDP) {
		const typeof(cnode->ether.type.ipv4.udp) *cfg =
			&cnode->ether.type.ipv4.udp;
		return source ? (cfg->src_range > 0 ? (uint32_t)cfg->src_range : cfg->n_src)
			: (cfg->dst_range > 0 ? (uint32_t)cfg->dst_range : cfg->n_dst);
	}

	const char *name = bless_get_type_name(type_idx);
	for (uint8_t i = 0; i < cnode->n_ext; i++) {
		const struct bless_ext_cfg *desc = cnode->ext[i].desc;
		if (!desc || !desc->name || !desc->port_range || !cnode->ext[i].cfg ||
		    strcmp(desc->name, name) != 0) {
			continue;
		}
		int32_t src_range = 0, dst_range = 0;
		uint16_t n_src = 0, n_dst = 0;
		desc->port_range(cnode->ext[i].cfg, &src_range, &n_src,
			&dst_range, &n_dst);
		return source ? (src_range > 0 ? (uint32_t)src_range : n_src)
			: (dst_range > 0 ? (uint32_t)dst_range : n_dst);
	}
	return 1; /* protocols without a port are observed as the fixed value zero */
}

static double
port_population_target(const struct dist_ratio *dr, const Cnode *cnode,
		       int source, double *shannon_upper, int *available)
{
	uint32_t weights[BLESS_MAX_TYPES] = {0};
	uint32_t pools[BLESS_MAX_TYPES] = {0};
	uint64_t total = 0;

	for (unsigned type = 0; type < BLESS_MAX_TYPES; type++) {
		int32_t weight = type_weight(dr, type);
		if (!weight) {
			continue;
		}
		weights[type] = (uint32_t)weight;
		pools[type] = configured_port_pool(cnode, type, source);
		total += (uint32_t)weight;
	}
	if (!total) {
		*available = 0;
		*shannon_upper = 0.0;
		return 0.0;
	}

	*available = 1;
	double target = entropy_weighted_pool_targets(weights, pools,
		BLESS_MAX_TYPES, shannon_upper);
	*shannon_upper = fmin(16.0, *shannon_upper);
	return target;
}

static double
imix_target(const Cnode *cnode, int *available)
{
	uint16_t n = cnode->ether.n_imix;
	*available = n > 0;
	if (!n) {
		return 0.0;
	}
	uint16_t largest = 0;
	for (uint16_t i = 0; i < n; i++) {
		uint16_t count = 0;
		for (uint16_t j = 0; j < n; j++)
			if (cnode->ether.imix[i] == cnode->ether.imix[j]) {
				count++;
			}
		if (count > largest) {
			largest = count;
		}
	}
	return -log2((double)largest / (double)n);
}

/*
 * compute_entropy_stats -- drain per-worker entropy samplers, build
 * empirical frequency histograms, and fill entropy_H fields.
 *
 * Shannon entropy H(X) = -Sigma p_i . log2(p_i) over observed values.
 * Min-entropy H_inf(X) = -log2(max p_i), also computed per dimension.
 * Diagnostic baselines are configuration-derived where faithful and otherwise
 * use observed support; an empty population is reported as inactive.
 */
void
compute_entropy_stats(struct stats_snapshot *s,
		      const struct dist_ratio *dr, const Cnode *cnode,
		      struct bless_conf *bconf)
{
	uint32_t cfg_n_src_ip = 0, cfg_n_dst_ip = 0;
	uint32_t cfg_n_src_pt = 0, cfg_n_dst_pt = 0;
	uint32_t cfg_n_vx_src = 0, cfg_n_vx_dst = 0, cfg_n_vni = 0;
	double cfg_min_src_port = 0.0, cfg_min_dst_port = 0.0;
	int cfg_src_port_available = 0, cfg_dst_port_available = 0;
	int32_t configured_weights[BLESS_MAX_TYPES] = {0};
	for (unsigned type = 0; type < BLESS_MAX_TYPES; type++)
		configured_weights[type] = type_weight(dr, type);

	/* stack buffers -- cap per dimension to avoid blowing the stack */
#define ENT_MAX  ENTROPY_RING_SIZE

	uint32_t protos[ENT_MAX];
	uint32_t src_ips[ENT_MAX];
	uint32_t dst_ips[ENT_MAX];
	uint32_t src_pts[ENT_MAX];
	uint32_t dst_pts[ENT_MAX];
	uint32_t pkt_sizes[ENT_MAX];
	uint32_t vxlan_flags[ENT_MAX];
	uint32_t outer_sips[ENT_MAX];
	uint32_t outer_dips[ENT_MAX];
	uint32_t outer_spts[ENT_MAX];
	uint32_t vnis[ENT_MAX];
	uint32_t tcp_flags_arr[ENT_MAX];  /* TCP flag bytes */
	/* new dimensions */
	uint64_t deltas[ENT_MAX];      /* delta_tsc (timing) */
	uint32_t joint_keys[ENT_MAX];  /* joint 5-tuple (cross-dimension) */
	/* joint keys for mutual information (64-bit packed pairs) */
	uint64_t joint_sip_dip[ENT_MAX];  /* (src_ip << 32) | dst_ip */
	uint64_t joint_spt_dpt[ENT_MAX];  /* (src_port << 32) | dst_port */
	uint64_t joint_proto_spt[ENT_MAX];/* (proto << 32) | src_port */
	/* Tier 2 joint keys */
	uint64_t joint_size_dpt[ENT_MAX]; /* (pkt_size << 32) | dst_port */
	uint64_t joint_size_proto[ENT_MAX];/* (pkt_size << 32) | proto */
	uint64_t joint_dtsc_proto[ENT_MAX];/* (delta_tsc >> 12) << 32) | proto */
	/* Interleave MI: (delta_tsc, flow_key) -- I(Dt; flow) */
	uint64_t joint_dtsc_flow[ENT_MAX];/* (delta_tsc >> 12 << 32) | flow_key */
	/* Tier 3: TCP flag joint keys (only when proto==6) */
	uint64_t joint_tcpf_sz[ENT_MAX];  /* (tcp_flags << 32) | pkt_size */
	uint64_t joint_tcpf_spt[ENT_MAX]; /* (tcp_flags << 32) | src_port */
	uint64_t joint_tcpf_dpt[ENT_MAX]; /* (tcp_flags << 32) | dst_port */
	/* Tier 4: VXLAN joint keys (only when vxlan flags set) */
	uint64_t joint_osip_odip[ENT_MAX];/* (outer_src_ip << 32) | outer_dst_ip */
	uint64_t joint_vni_osip[ENT_MAX]; /* (vni << 32) | outer_src_ip */
	size_t np = 0, nsip = 0, ndip = 0, nspt = 0, ndpt = 0;
	size_t nsz = 0, nvf = 0, nosip = 0, nodig = 0, nospt = 0, nvni = 0;
	size_t ntcpf = 0;
	size_t ndel = 0, njk = 0;
	size_t njd_sd = 0, njd_pd = 0, njd_ps = 0;  /* joint dim counters */
	size_t njd_szd = 0, njd_szp = 0, njd_dtp = 0; /* Tier 2 joint counters */
	size_t njd_df = 0; /* I(Dt; flow) */
	size_t njd_tfsz = 0, njd_tfsp = 0, njd_tfdp = 0; /* Tier 3 */
	size_t njd_osod = 0, njd_vnos = 0; /* Tier 4 */

	/* flow tracking accumulators */
	uint64_t flow_total_pkts_all = 0;
	uint8_t flow_hll[ENTROPY_FLOW_HLL_REGISTERS] = {0};
	uint64_t sampler_overwritten_total = 0;
	uint64_t sampler_overwritten_window = 0;

	unsigned main_lc = rte_get_main_lcore();

	for (unsigned lc = 0; lc < RTE_MAX_LCORE; lc++) {
		struct entropy_sampler *sam = entropy_samplers[lc];
		if (!sam || lc == main_lc) {
			continue;
		}

		uint32_t wi = __atomic_load_n(&sam->write_idx,
					      __ATOMIC_ACQUIRE);
		uint32_t last = sam->last_read_idx;
		uint64_t overwritten = 0;
		uint32_t nnew = entropy_sampler_read_window(wi, &last,
			ENTROPY_RING_SIZE, &overwritten);
		sam->overwritten += overwritten;
		sampler_overwritten_window += overwritten;

		for (uint32_t i = 0; i < nnew && i < ENTROPY_RING_SIZE; i++) {
			struct entropy_5tuple *t =
				&sam->samples[(last + i) % ENTROPY_RING_SIZE];
			if (np < ENT_MAX) {
				protos[np++] = t->proto;
			}
			if (nsip < ENT_MAX) {
				src_ips[nsip++] = t->src_ip;
			}
			if (ndip < ENT_MAX) {
				dst_ips[ndip++] = t->dst_ip;
			}
			if (nspt < ENT_MAX) {
				src_pts[nspt++] = t->src_port;
			}
			if (ndpt < ENT_MAX) {
				dst_pts[ndpt++] = t->dst_port;
			}
			/* packet size & VXLAN dimensions */
			if (nsz < ENT_MAX) {
				pkt_sizes[nsz++] = t->pkt_size;
			}
			if (nvf < ENT_MAX) {
				vxlan_flags[nvf++] = t->flags;
			}
			if (t->flags & 0x01) {
				if (nosip < ENT_MAX) {
					outer_sips[nosip++] = t->outer_src_ip;
				}
				if (nodig < ENT_MAX) {
					outer_dips[nodig++] = t->outer_dst_ip;
				}
				if (nospt < ENT_MAX) {
					outer_spts[nospt++] = t->outer_src_port;
				}
				if (nvni < ENT_MAX) {
					vnis[nvni++] = t->vni;
				}
				/* Tier 4: I(outer_src_ip; outer_dst_ip) */
				if (njd_osod < ENT_MAX) {
					joint_osip_odip[njd_osod++] =
					    ((uint64_t)t->outer_src_ip << 32) | t->outer_dst_ip;
				}
				/* Tier 4: I(vni; outer_src_ip) */
				if (njd_vnos < ENT_MAX) {
					joint_vni_osip[njd_vnos++] =
					    ((uint64_t)t->vni << 32) | t->outer_src_ip;
				}
			}
			/* TCP flags (valid for proto==6) */
			if (t->proto == 6 && ntcpf < ENT_MAX) {
				tcp_flags_arr[ntcpf++] = t->tcp_flags;
				/* Tier 3: I(tcp_flags; pkt_size) */
				if (njd_tfsz < ENT_MAX) {
					joint_tcpf_sz[njd_tfsz++] =
					    ((uint64_t)t->tcp_flags << 32) | t->pkt_size;
				}
				/* Tier 3: I(tcp_flags; src_port) */
				if (njd_tfsp < ENT_MAX) {
					joint_tcpf_spt[njd_tfsp++] =
					    ((uint64_t)t->tcp_flags << 32) | t->src_port;
				}
				/* Tier 3: I(tcp_flags; dst_port) */
				if (njd_tfdp < ENT_MAX) {
					joint_tcpf_dpt[njd_tfdp++] =
					    ((uint64_t)t->tcp_flags << 32) | t->dst_port;
				}
			}
			/* timing entropy: store TSC delta */
			if (ndel < ENT_MAX) {
				/* convert 64-bit to 32-bit by keeping ~usec precision;
				 * timing entropy typically lives in the 0-100us range.
				 * Right-shift by ~TSC Hz / 1e6 ~ 2.5 GHz/1e6 ~ 2500 ~ 2^11.
				 * Shift by 12 gives ~0.4us resolution -- good enough. */
				deltas[ndel++] = (uint32_t)(t->delta_tsc >> 12);
			}
			/* joint entropy key */
			if (njk < ENT_MAX) {
				joint_keys[njk++] = t->joint_key;
			}
			/* mutual information: (src_ip, dst_ip) */
			if (njd_sd < ENT_MAX) {
				joint_sip_dip[njd_sd++] =
				    ((uint64_t)t->src_ip << 32) | t->dst_ip;
			}
			/* mutual information: (src_port, dst_port) */
			if (njd_pd < ENT_MAX) {
				joint_spt_dpt[njd_pd++] =
				    ((uint64_t)t->src_port << 32) | t->dst_port;
			}
			/* mutual information: (protocol, src_port) */
			if (njd_ps < ENT_MAX) {
				joint_proto_spt[njd_ps++] =
				    ((uint64_t)t->proto << 32) | t->src_port;
			}
			/* Tier 2 mutual information: (pkt_size, dst_port) */
			if (njd_szd < ENT_MAX) {
				joint_size_dpt[njd_szd++] =
				    ((uint64_t)t->pkt_size << 32) | t->dst_port;
			}
			/* Tier 2 mutual information: (pkt_size, protocol) */
			if (njd_szp < ENT_MAX) {
				joint_size_proto[njd_szp++] =
				    ((uint64_t)t->pkt_size << 32) | t->proto;
			}
			/* Tier 2 mutual information: (delta_tsc, protocol) */
			if (njd_dtp < ENT_MAX) {
				joint_dtsc_proto[njd_dtp++] =
				    (((uint64_t)(t->delta_tsc >> 12)) << 32) | t->proto;
			}
			/* Interleave MI: (delta_tsc, flow_key) -- I(Dt; flow) */
			if (njd_df < ENT_MAX) {
				joint_dtsc_flow[njd_df++] =
				    (((uint64_t)(t->delta_tsc >> 12)) << 32) | t->flow_key;
			}
		}

		/* Read cumulative flow and overwrite counters from this sampler. */
		flow_total_pkts_all += __atomic_load_n(&sam->flow_total_pkts,
							 __ATOMIC_RELAXED);
		for (unsigned i = 0; i < ENTROPY_FLOW_HLL_REGISTERS; i++) {
			uint8_t rank = atomic_load_explicit(&sam->flow_hll[i],
				memory_order_relaxed);
			if (rank > flow_hll[i]) {
				flow_hll[i] = rank;
			}
		}
		sampler_overwritten_total += sam->overwritten;

		sam->last_read_idx = wi;
	}

	/* sort each dimension and compute Shannon H + min-entropy */
	if (np > 1) {
		qsort(protos, np, sizeof(uint32_t), cmp_u32);
	}
	if (nsip > 1) {
		qsort(src_ips, nsip, sizeof(uint32_t), cmp_u32);
	}
	if (ndip > 1) {
		qsort(dst_ips, ndip, sizeof(uint32_t), cmp_u32);
	}
	if (nspt > 1) {
		qsort(src_pts, nspt, sizeof(uint32_t), cmp_u32);
	}
	if (ndpt > 1) {
		qsort(dst_pts, ndpt, sizeof(uint32_t), cmp_u32);
	}
	if (nsz > 1) {
		qsort(pkt_sizes, nsz, sizeof(uint32_t), cmp_u32);
	}
	if (nvf > 1) {
		qsort(vxlan_flags, nvf, sizeof(uint32_t), cmp_u32);
	}
	if (nosip > 1) {
		qsort(outer_sips, nosip, sizeof(uint32_t), cmp_u32);
	}
	if (nodig > 1) {
		qsort(outer_dips, nodig, sizeof(uint32_t), cmp_u32);
	}
	if (nospt > 1) {
		qsort(outer_spts, nospt, sizeof(uint32_t), cmp_u32);
	}
	if (nvni > 1) {
		qsort(vnis, nvni, sizeof(uint32_t), cmp_u32);
	}
	if (ntcpf > 1) {
		qsort(tcp_flags_arr, ntcpf, sizeof(uint32_t), cmp_u32);
	}
	if (ndel > 1) {
		qsort(deltas, ndel, sizeof(uint64_t), cmp_u64);
	}
	if (njk > 1) {
		qsort(joint_keys, njk, sizeof(uint32_t), cmp_u32);
	}
	if (njd_sd > 1) {
		qsort(joint_sip_dip, njd_sd, sizeof(uint64_t), cmp_u64);
	}
	if (njd_pd > 1) {
		qsort(joint_spt_dpt, njd_pd, sizeof(uint64_t), cmp_u64);
	}
	if (njd_ps > 1) {
		qsort(joint_proto_spt, njd_ps, sizeof(uint64_t), cmp_u64);
	}
	if (njd_szd > 1) {
		qsort(joint_size_dpt, njd_szd, sizeof(uint64_t), cmp_u64);
	}
	if (njd_szp > 1) {
		qsort(joint_size_proto, njd_szp, sizeof(uint64_t), cmp_u64);
	}
	if (njd_dtp > 1) {
		qsort(joint_dtsc_proto, njd_dtp, sizeof(uint64_t), cmp_u64);
	}
	if (njd_df > 1) {
		qsort(joint_dtsc_flow, njd_df, sizeof(uint64_t), cmp_u64);
	}
	if (njd_tfsz > 1) {
		qsort(joint_tcpf_sz, njd_tfsz, sizeof(uint64_t), cmp_u64);
	}
	if (njd_tfsp > 1) {
		qsort(joint_tcpf_spt, njd_tfsp, sizeof(uint64_t), cmp_u64);
	}
	if (njd_tfdp > 1) {
		qsort(joint_tcpf_dpt, njd_tfdp, sizeof(uint64_t), cmp_u64);
	}
	if (njd_osod > 1) {
		qsort(joint_osip_odip, njd_osod, sizeof(uint64_t), cmp_u64);
	}
	if (njd_vnos > 1) {
		qsort(joint_vni_osip, njd_vnos, sizeof(uint64_t), cmp_u64);
	}

#define SHANNON(arr, n) \
	((n) > 1 ? shannon_from_sorted(arr, n, sizeof(uint32_t), cmp_u32, NULL) : 0.0)
#define SHANNON_AND_MIN(arr, n, min_out) \
	((n) > 1 ? shannon_from_sorted(arr, n, sizeof(uint32_t), cmp_u32, &(min_out)) : \
	 ((min_out)=0.0, 0.0))
#define SHANNON_U64(arr, n) \
	((n) > 1 ? shannon_from_sorted(arr, n, sizeof(uint64_t), cmp_u64, NULL) : 0.0)
#define SHANNON_AND_MIN_U64(arr, n, min_out) \
	((n) > 1 ? shannon_from_sorted(arr, n, sizeof(uint64_t), cmp_u64, &(min_out)) : \
	 ((min_out)=0.0, 0.0))

#define H_FIELD(joint, n, upper) ({ \
	double _h_ = 0.0; \
	if ((n) > 1) { \
		uint32_t *_tmp_ = malloc((n) * sizeof(uint32_t)); \
		if (_tmp_) { \
			for (int _i_ = 0; _i_ < (n); _i_++) \
				_tmp_[_i_] = (upper) \
					? (uint32_t)((joint)[_i_] >> 32) \
					: (uint32_t)(joint)[_i_]; \
			qsort(_tmp_, (n), sizeof(uint32_t), cmp_u32); \
			_h_ = shannon_from_sorted(_tmp_, (n), sizeof(uint32_t), cmp_u32, NULL); \
			free(_tmp_); \
		} \
	} \
	_h_; \
})

/* Shannon entropy (all dimensions) */	s->entropy_protocol       = SHANNON(protos,     np);
	s->entropy_src_ip         = SHANNON(src_ips,    nsip);
	s->entropy_dst_ip         = SHANNON(dst_ips,    ndip);
	s->entropy_src_port       = SHANNON(src_pts,    nspt);
	s->entropy_dst_port       = SHANNON(dst_pts,    ndpt);
	s->entropy_pkt_size       = SHANNON(pkt_sizes,   nsz);
	s->entropy_vxlan_encap    = SHANNON(vxlan_flags, nvf);
	s->entropy_outer_src_ip   = SHANNON(outer_sips,  nosip);
	s->entropy_outer_dst_ip   = SHANNON(outer_dips,  nodig);
	s->entropy_outer_src_port = SHANNON(outer_spts,  nospt);
	s->entropy_vni            = SHANNON(vnis,        nvni);
	s->entropy_tcp_flags      = SHANNON(tcp_flags_arr, ntcpf);
	/* P0a: timing entropy */
	s->entropy_delta_tsc      = SHANNON_U64(deltas,  ndel);
	/* P1: joint 5-tuple entropy */
	s->entropy_joint_5tuple   = SHANNON(joint_keys,  njk);

	/* mutual information I(X;Y) = H(X) + H(Y) - H(X,Y) */	{
		double h_sd = SHANNON_U64(joint_sip_dip, njd_sd);
		s->mi_sip_dip = (njd_sd > 1 && nsip > 1 && ndip > 1)
			? s->entropy_src_ip + s->entropy_dst_ip - h_sd : 0.0;

		double h_pd = SHANNON_U64(joint_spt_dpt, njd_pd);
		s->mi_spt_dpt = (njd_pd > 1 && nspt > 1 && ndpt > 1)
			? s->entropy_src_port + s->entropy_dst_port - h_pd : 0.0;

		double h_ps = SHANNON_U64(joint_proto_spt, njd_ps);
		s->mi_proto_spt = (njd_ps > 1 && np > 1 && nspt > 1)
			? s->entropy_protocol + s->entropy_src_port - h_ps : 0.0;

		/* Tier 2: I(pkt_size; dst_port) */
		double h_szd = SHANNON_U64(joint_size_dpt, njd_szd);
		s->mi_size_dpt = (njd_szd > 1 && nsz > 1 && ndpt > 1)
			? s->entropy_pkt_size + s->entropy_dst_port - h_szd : 0.0;

		/* Tier 2: I(pkt_size; protocol) */
		double h_szp = SHANNON_U64(joint_size_proto, njd_szp);
		s->mi_size_proto = (njd_szp > 1 && nsz > 1 && np > 1)
			? s->entropy_pkt_size + s->entropy_protocol - h_szp : 0.0;

		/* Tier 2: I(delta_tsc; protocol) */
		double h_dtp = SHANNON_U64(joint_dtsc_proto, njd_dtp);
		s->mi_dtsc_proto = (njd_dtp > 1 && ndel > 1 && np > 1)
			? s->entropy_delta_tsc + s->entropy_protocol - h_dtp : 0.0;

		/* I(Dt; flow_key) -- temporal flow locality; reduced by interleave */
		if (njd_df > 1) {
			double h_df    = SHANNON_U64(joint_dtsc_flow, njd_df);
			double h_d_flow = H_FIELD(joint_dtsc_flow, njd_df, 1);
			double h_flow_d = H_FIELD(joint_dtsc_flow, njd_df, 0);
			s->mi_dtsc_flow = h_d_flow + h_flow_d - h_df;
			s->mi_max_dtsc_flow = fmin(h_d_flow, h_flow_d);
		} else {
			s->mi_dtsc_flow = 0.0;
			s->mi_max_dtsc_flow = 0.0;
		}

		/* Tier 3: I(tcp_flags; pkt_size) -- marginals from joint to ensure same population */
		double h_tfsz = SHANNON_U64(joint_tcpf_sz, njd_tfsz);
		double h_tf_sz  = H_FIELD(joint_tcpf_sz, njd_tfsz, 1); /* upper = tcp_flags */
		double h_sz_tf  = H_FIELD(joint_tcpf_sz, njd_tfsz, 0); /* lower = pkt_size */
		s->mi_tcpf_sz = (njd_tfsz > 1)
			? h_tf_sz + h_sz_tf - h_tfsz : 0.0;
		s->mi_max_tcpf_sz = fmin(h_tf_sz, h_sz_tf);
		/* Tier 3: I(tcp_flags; src_port) -- marginals from joint */
		double h_tfsp = SHANNON_U64(joint_tcpf_spt, njd_tfsp);
		double h_tf_sp  = H_FIELD(joint_tcpf_spt, njd_tfsp, 1); /* upper = tcp_flags */
		double h_sp_tf  = H_FIELD(joint_tcpf_spt, njd_tfsp, 0); /* lower = src_port */
		s->mi_tcpf_spt = (njd_tfsp > 1)
			? h_tf_sp + h_sp_tf - h_tfsp : 0.0;
		s->mi_max_tcpf_spt = fmin(h_tf_sp, h_sp_tf);
		/* Tier 3: I(tcp_flags; dst_port) -- marginals from joint */
		double h_tfdp = SHANNON_U64(joint_tcpf_dpt, njd_tfdp);
		double h_tf_dp  = H_FIELD(joint_tcpf_dpt, njd_tfdp, 1); /* upper = tcp_flags */
		double h_dp_tf  = H_FIELD(joint_tcpf_dpt, njd_tfdp, 0); /* lower = dst_port */
		s->mi_tcpf_dpt = (njd_tfdp > 1)
			? h_tf_dp + h_dp_tf - h_tfdp : 0.0;
		s->mi_max_tcpf_dpt = fmin(h_tf_dp, h_dp_tf);

		/* Tier 4: I(outer_src_ip; outer_dst_ip) */
		double h_osod = SHANNON_U64(joint_osip_odip, njd_osod);
		s->mi_osip_odip = (njd_osod > 1 && nosip > 1 && nodig > 1)
			? s->entropy_outer_src_ip + s->entropy_outer_dst_ip - h_osod : 0.0;
		/* Tier 4: I(vni; outer_src_ip) */
		double h_vnos = SHANNON_U64(joint_vni_osip, njd_vnos);
		s->mi_vni_osip = (njd_vnos > 1 && nvni > 1 && nosip > 1)
			? s->entropy_vni + s->entropy_outer_src_ip - h_vnos : 0.0;
	}

	/* Compute Shannon values while writing min-entropy through the output
	 * parameter. Do not assign the return value to the min field. */
	(void)SHANNON_AND_MIN(protos, np, s->entropy_min_protocol);
	(void)SHANNON_AND_MIN(src_ips, nsip, s->entropy_min_src_ip);
	(void)SHANNON_AND_MIN(dst_ips, ndip, s->entropy_min_dst_ip);
	(void)SHANNON_AND_MIN(src_pts, nspt, s->entropy_min_src_port);
	(void)SHANNON_AND_MIN(dst_pts, ndpt, s->entropy_min_dst_port);
	(void)SHANNON_AND_MIN(pkt_sizes, nsz, s->entropy_min_pkt_size);
	(void)SHANNON_AND_MIN(tcp_flags_arr, ntcpf, s->entropy_min_tcp_flags);
	(void)SHANNON_AND_MIN_U64(deltas, ndel, s->entropy_min_delta_tsc);
	(void)SHANNON_AND_MIN(outer_sips, nosip, s->entropy_min_outer_src_ip);
	(void)SHANNON_AND_MIN(outer_dips, nodig, s->entropy_min_outer_dst_ip);
	(void)SHANNON_AND_MIN(vnis, nvni, s->entropy_min_vni);
	/* total min-entropy: sum of per-dimension min-entropy */
	s->entropy_min_total_5tuple  = s->entropy_min_protocol
		+ s->entropy_min_src_ip + s->entropy_min_dst_ip
		+ s->entropy_min_src_port + s->entropy_min_dst_port
		+ s->entropy_min_pkt_size + s->entropy_min_tcp_flags;

	/* combined total */	s->entropy_total_5tuple = s->entropy_protocol + s->entropy_src_ip
		+ s->entropy_dst_ip + s->entropy_src_port + s->entropy_dst_port
		+ s->entropy_pkt_size + s->entropy_tcp_flags;

	/* P0b: merge per-worker HyperLogLog registers. */
	double hll_estimate = entropy_flow_hll_bounded_estimate(flow_hll,
		flow_total_pkts_all);
	s->flow_distinct = hll_estimate;
	s->flow_total    = (double)flow_total_pkts_all;
	s->flow_ratio    = flow_total_pkts_all > 0
		? fmin(1.0, hll_estimate / (double)flow_total_pkts_all) : 0.0;
	s->sampler_samples = (double)np;
	s->sampler_overwritten = (double)sampler_overwritten_total;
	s->sampler_overwritten_window = (double)sampler_overwritten_window;

	/* P0c: max possible entropy from config */	{
		/* src IP: range or explicit array */
		if (cnode->ether.type.ipv4.src_range > 0) {
			cfg_n_src_ip = (uint32_t)cnode->ether.type.ipv4.src_range;
		}
		if (cfg_n_src_ip == 0 && cnode->ether.type.ipv4.n_src > 0) {
			cfg_n_src_ip = cnode->ether.type.ipv4.n_src;
		}

		/* dst IP */
		if (cnode->ether.type.ipv4.dst_range > 0) {
			cfg_n_dst_ip = (uint32_t)cnode->ether.type.ipv4.dst_range;
		} else if (cnode->ether.type.ipv4.n_dst > 0) {
			cfg_n_dst_ip = cnode->ether.type.ipv4.n_dst;
		}

		/* src port -- TCP or UDP (protocol mixed, take max) */
		if (cnode->ether.type.ipv4.tcp.src_range > 0) {
			cfg_n_src_pt = (uint32_t)cnode->ether.type.ipv4.tcp.src_range;
		} else if (cnode->ether.type.ipv4.tcp.n_src > 0) {
			cfg_n_src_pt = (uint32_t)cnode->ether.type.ipv4.tcp.n_src;
		}
		if (cnode->ether.type.ipv4.udp.src_range > 0) {
			cfg_n_src_pt = (uint32_t)cnode->ether.type.ipv4.udp.src_range;
		} else if (cnode->ether.type.ipv4.udp.n_src > 0) {
			cfg_n_src_pt = (uint32_t)cnode->ether.type.ipv4.udp.n_src;
		}
		/* ext configs -- aggregate port ranges generically */
		bless_ext_aggregate_port_ranges(cnode, &cfg_n_src_pt, &cfg_n_dst_pt);

		/* dst port */
		if (cnode->ether.type.ipv4.tcp.dst_range > 0) {
			cfg_n_dst_pt = (uint32_t)cnode->ether.type.ipv4.tcp.dst_range;
		} else if (cnode->ether.type.ipv4.tcp.n_dst > 0) {
			cfg_n_dst_pt = (uint32_t)cnode->ether.type.ipv4.tcp.n_dst;
		}
		if (cnode->ether.type.ipv4.udp.dst_range > 0) {
			cfg_n_dst_pt = (uint32_t)cnode->ether.type.ipv4.udp.dst_range;
		} else if (cnode->ether.type.ipv4.udp.n_dst > 0) {
			cfg_n_dst_pt = (uint32_t)cnode->ether.type.ipv4.udp.n_dst;
		}

		s->max_src_ip   = cfg_n_src_ip > 0 ? log2((double)cfg_n_src_ip)   : 32.0;
		s->max_dst_ip   = cfg_n_dst_ip > 0 ? log2((double)cfg_n_dst_ip)   : 32.0;
		cfg_min_src_port = port_population_target(dr, cnode, 1,
			&s->max_src_port, &cfg_src_port_available);
		cfg_min_dst_port = port_population_target(dr, cnode, 0,
			&s->max_dst_port, &cfg_dst_port_available);
		if (!cfg_src_port_available) {
			s->max_src_port = cfg_n_src_pt > 0 ? log2((double)cfg_n_src_pt) : 16.0;
		}
		if (!cfg_dst_port_available) {
			s->max_dst_port = cfg_n_dst_pt > 0 ? log2((double)cfg_n_dst_pt) : 16.0;
		}

		/* VXLAN outer IP / VNI ranges */
		{
			const typeof(cnode->vxlan.ether.type.ipv4) *vx =
				&cnode->vxlan.ether.type.ipv4;
			cfg_n_vx_src = vx->src_range > 0 ? (uint32_t)vx->src_range
				: (uint32_t)entropy_count_distinct_u32(vx->src, vx->n_src);
			cfg_n_vx_dst = vx->dst_range > 0 ? (uint32_t)vx->dst_range
				: (uint32_t)entropy_count_distinct_u32(vx->dst, vx->n_dst);
			cfg_n_vni = (uint32_t)entropy_count_distinct_u32(vx->vni, vx->n_src);
			s->max_outer_src_ip = cfg_n_vx_src > 0 ? log2((double)cfg_n_vx_src) : 0.0;
			s->max_outer_dst_ip = cfg_n_vx_dst > 0 ? log2((double)cfg_n_vx_dst) : 0.0;
			s->max_vni          = cfg_n_vni > 0 ? log2((double)cfg_n_vni) : 0.0;
		}
	}

	/* I(X;Y) cannot exceed min(H(X), H(Y)). Observed marginal entropy is
	 * the valid bound when mutation expands support beyond the config pool. */
	s->mi_max_sip_dip    = fmin(s->entropy_src_ip, s->entropy_dst_ip);
	s->mi_max_spt_dpt    = fmin(s->entropy_src_port, s->entropy_dst_port);
	s->mi_max_proto_spt  = fmin(s->entropy_protocol, s->entropy_src_port);
	s->mi_max_size_dpt   = fmin(s->entropy_pkt_size, s->entropy_dst_port);
	s->mi_max_size_proto = fmin(s->entropy_pkt_size, s->entropy_protocol);
	s->mi_max_dtsc_proto = fmin(s->entropy_delta_tsc, s->entropy_protocol);
	s->mi_max_osip_odip  = fmin(s->entropy_outer_src_ip,
		s->entropy_outer_dst_ip);
	s->mi_max_vni_osip   = fmin(s->entropy_vni, s->entropy_outer_src_ip);

	/* Configuration-aware min-entropy diagnostics. */
	{
		int configured = 0;
		double target = weighted_target(configured_weights, BLESS_MAX_TYPES,
			&configured);
		entropy_min_diag_fill_sorted(&s->min_diag[ENTROPY_MIN_PROTOCOL], s->entropy_min_protocol,
			protos, np, sizeof(protos[0]), configured, target);
		target = conditional_pool_target(configured_weights, BLESS_MAX_TYPES,
			cfg_n_src_ip, 1u << TYPE_ARP, &configured);
		entropy_min_diag_fill_sorted(&s->min_diag[ENTROPY_MIN_SRC_IP], s->entropy_min_src_ip,
			src_ips, nsip, sizeof(src_ips[0]), configured, target);
		target = conditional_pool_target(configured_weights, BLESS_MAX_TYPES,
			cfg_n_dst_ip, 1u << TYPE_ARP, &configured);
		entropy_min_diag_fill_sorted(&s->min_diag[ENTROPY_MIN_DST_IP], s->entropy_min_dst_ip,
			dst_ips, ndip, sizeof(dst_ips[0]), configured, target);
		target = cfg_min_src_port;
		configured = cfg_src_port_available;
		entropy_min_diag_fill_sorted(&s->min_diag[ENTROPY_MIN_SRC_PORT], s->entropy_min_src_port,
			src_pts, nspt, sizeof(src_pts[0]), configured, target);
		target = cfg_min_dst_port;
		configured = cfg_dst_port_available;
		entropy_min_diag_fill_sorted(&s->min_diag[ENTROPY_MIN_DST_PORT], s->entropy_min_dst_port,
			dst_pts, ndpt, sizeof(dst_pts[0]), configured, target);
		target = imix_target(cnode, &configured);
		if (positive_weights(configured_weights, BLESS_MAX_TYPES) != 1) {
			configured = 0;
		}
		entropy_min_diag_fill_sorted(&s->min_diag[ENTROPY_MIN_PKT_SIZE], s->entropy_min_pkt_size,
			pkt_sizes, nsz, sizeof(pkt_sizes[0]), configured, target);
		entropy_min_diag_fill_sorted(&s->min_diag[ENTROPY_MIN_TCP_FLAGS], s->entropy_min_tcp_flags,
			tcp_flags_arr, ntcpf, sizeof(tcp_flags_arr[0]), 0, 0.0);
		entropy_min_diag_fill_sorted(&s->min_diag[ENTROPY_MIN_DELTA_TSC], s->entropy_min_delta_tsc,
			deltas, ndel, sizeof(deltas[0]), 0, 0.0);
		entropy_min_diag_fill_sorted(&s->min_diag[ENTROPY_MIN_OUTER_SRC_IP],
			s->entropy_min_outer_src_ip, outer_sips, nosip, sizeof(outer_sips[0]),
			cfg_n_vx_src > 0, s->max_outer_src_ip);
		entropy_min_diag_fill_sorted(&s->min_diag[ENTROPY_MIN_OUTER_DST_IP],
			s->entropy_min_outer_dst_ip, outer_dips, nodig, sizeof(outer_dips[0]),
			cfg_n_vx_dst > 0, s->max_outer_dst_ip);
		entropy_min_diag_fill_sorted(&s->min_diag[ENTROPY_MIN_VNI], s->entropy_min_vni,
			vnis, nvni, sizeof(vnis[0]), cfg_n_vni > 0, s->max_vni);

		struct entropy_min_diagnostic *total =
			&s->min_diag[ENTROPY_MIN_TOTAL_5TUPLE];
		memset(total, 0, sizeof(*total));
		total->measured = s->entropy_min_total_5tuple;
		total->samples = np;
		total->baseline_source = ENTROPY_BASELINE_CONFIGURED;
		const unsigned components[] = { ENTROPY_MIN_PROTOCOL, ENTROPY_MIN_SRC_IP,
			ENTROPY_MIN_DST_IP, ENTROPY_MIN_SRC_PORT, ENTROPY_MIN_DST_PORT,
			ENTROPY_MIN_PKT_SIZE, ENTROPY_MIN_TCP_FLAGS };
		for (unsigned i = 0; i < sizeof(components) / sizeof(components[0]); i++) {
			struct entropy_min_diagnostic *part = &s->min_diag[components[i]];
		total->target += part->target;
		total->population_target += part->population_target;
		if (part->baseline_source != ENTROPY_BASELINE_CONFIGURED) {
			total->baseline_source = ENTROPY_BASELINE_MIXED;
		}
		}
		total->gap_bits = fmax(0.0, total->target - total->measured);
		total->attainment = total->target > 0.0
			? fmin(1.0, total->measured / total->target) : 1.0;
		total->dominance_ratio = exp2(total->gap_bits);
		if (!total->samples) {
			total->state = ENTROPY_DIAG_INACTIVE;
		} else if (total->samples < ENTROPY_DIAG_MIN_SAMPLES) {
			total->state = ENTROPY_DIAG_INSUFFICIENT_SAMPLES;
		} else if (total->baseline_source != ENTROPY_BASELINE_CONFIGURED) {
			total->state = ENTROPY_DIAG_INFORMATIONAL;
		} else if (total->attainment >= 0.90) {
			total->state = ENTROPY_DIAG_GOOD;
		} else if (total->attainment >= 0.70) {
			total->state = ENTROPY_DIAG_DEGRADED;
		} else {
			total->state = ENTROPY_DIAG_POOR;
		}
	}
	/* latency histogram aggregation (per-worker -> total) */	{
		struct latency_hist total_lat;
		latency_hist_reset(&total_lat);
		unsigned main_lc = rte_get_main_lcore();
		for (unsigned lc = 0; lc < RTE_MAX_LCORE; lc++) {
			struct entropy_sampler *sam = entropy_samplers[lc];
			if (!sam || lc == main_lc) {
				continue;
			}
			struct latency_hist *lh = &sam->lat_hist;
			for (int bi = 0; bi < LAT_HIST_BUCKETS; bi++) {
				total_lat.count[bi] += __atomic_load_n(&lh->count[bi], __ATOMIC_RELAXED);
				__atomic_store_n(&lh->count[bi], 0, __ATOMIC_RELAXED);
			}
			total_lat.total += lh->total;
			lh->total = 0;
		}
		s->lat_samples = total_lat.total;
		if (total_lat.total > 0) {
			s->lat_p50  = latency_hist_percentile(&total_lat, 0.50);
			s->lat_p95  = latency_hist_percentile(&total_lat, 0.95);
			s->lat_p99  = latency_hist_percentile(&total_lat, 0.99);
			s->lat_p999 = latency_hist_percentile(&total_lat, 0.999);
		} else {
			s->lat_p50 = s->lat_p95 = s->lat_p99 = s->lat_p999 = 0.0;
		}
	}

	/* MI diagonal smoothing (EMA) */	if (bconf && bconf->mi_smoothing_window > 1) {
		double alpha = 2.0 / (double)(bconf->mi_smoothing_window + 1);
		/* map dimension index -> stats_snapshot field */
		double *cur[] = {
			&s->entropy_protocol,   &s->entropy_src_ip,
			&s->entropy_dst_ip,     &s->entropy_src_port,
			&s->entropy_dst_port,   &s->entropy_pkt_size,
			&s->entropy_delta_tsc,  &s->entropy_tcp_flags,
			&s->entropy_joint_5tuple, &s->entropy_vxlan_encap,
			&s->entropy_outer_src_ip, &s->entropy_vni,
		};
		for (int i = 0; i < 12; i++) {
			double prev = bconf->mi_smoothed[i];
			/* first sample: initialise smoothed from current */
			if (prev == 0.0 && *cur[i] > 0.0) {
				prev = *cur[i];
			}
			double smoothed = alpha * (*cur[i]) + (1.0 - alpha) * prev;
			bconf->mi_smoothed[i] = smoothed;
			*cur[i] = smoothed;
		}
	}

#undef SHANNON_AND_MIN_U64
#undef SHANNON_AND_MIN
#undef SHANNON_U64
#undef SHANNON
#undef H_FIELD
#undef ENT_MAX
}
