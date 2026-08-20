#ifndef __ENTROPY_H__
#define __ENTROPY_H__

#include <stdint.h>
#include <stdatomic.h>
#include <math.h>
#include <string.h>

#include <rte_byteorder.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_tcp.h>
#include <rte_udp.h>
#include <rte_mbuf.h>
#include <rte_lcore.h>
#include <rte_cycles.h>

#include "entropy_sampler_policy.h"
#include "entropy_math.h"

/* Forward — defined in bless_plugin.c */
extern uint8_t bless_pkt_ip_proto(unsigned int type_idx);

/* sampling ring */
#define ENTROPY_RING_SIZE  4096   /* per-worker circular buffer */

/* A single 5-tuple sample, extracted from a transmitted packet.
 * Inner (L4 gateway facing) tuple + outer VXLAN tuple when encapsulated.
 * Packed to minimise per-sample footprint in the ring buffer. */
struct entropy_5tuple {
	uint32_t src_ip;          /* inner src IP (network byte order) */
	uint32_t dst_ip;          /* inner dst IP */
	uint16_t src_port;        /* inner L4 src port (network byte order) */
	uint16_t dst_port;        /* inner L4 dst port */
	uint8_t  proto;           /* 1=ICMP, 6=TCP, 17=UDP, 255=ARP */
	uint8_t  tcp_flags;       /* TCP flags byte (valid when proto==6) */
	uint16_t pkt_size;        /* rte_mbuf->pkt_len (bytes on wire) */
	uint8_t  flags;           /* bit 0: vxlan-encapsulated */
	/* VXLAN outer fields (valid when flags & 0x01) */
	uint32_t outer_src_ip;    /* outer src IP */
	uint32_t outer_dst_ip;    /* outer dst IP */
	uint16_t outer_src_port;  /* outer UDP src port */
	uint32_t vni;             /* VXLAN Network Identifier (lower 24 bits) */
	/* new entropy dimensions */	uint64_t delta_tsc;       /* TSC delta from previous packet (timing entropy) */
	uint32_t flow_key;        /* XOR of 5-tuple fields for distinct flow counting */
	uint32_t joint_key;       /* compressed key: proto|src_hi|dst_hi|port_lo */
} __attribute__((packed));

#define LAT_HIST_BUCKETS  14
#define LAT_HIST_OVERFLOW (LAT_HIST_BUCKETS - 1)

/** Per-worker latency histogram (cache-line aligned). */
struct latency_hist {
	uint32_t count[LAT_HIST_BUCKETS] __rte_cache_aligned;
	uint64_t total;
};

/* Bucket thresholds in µs: 0,1,2,5,10,20,50,100,200,500,1000,2000,5000,10000 */
static const uint32_t lat_bucket_limits[LAT_HIST_BUCKETS] = {
	0, 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000
};

/** Record one latency sample (µs) into the histogram. */
static inline void
latency_hist_add(struct latency_hist *h, uint32_t us)
{
	h->total++;
	if (us >= lat_bucket_limits[LAT_HIST_BUCKETS - 1]) {
		h->count[LAT_HIST_BUCKETS - 1]++;
		return;
	}
	/* linear search -- LAT_HIST_BUCKETS=14, cheap */
	for (int i = LAT_HIST_BUCKETS - 2; i >= 0; i--) {
		if (us >= lat_bucket_limits[i]) {
			h->count[i]++;
			return;
		}
	}
	h->count[0]++; /* <1µs */
}

/** Compute a percentile from a latency histogram (0.0–1.0). */
static inline double
latency_hist_percentile(const struct latency_hist *h, double pct)
{
	if (h->total == 0) {
		return 0.0;
	}
	uint64_t target = (uint64_t)(pct * (double)h->total);
	if (target >= h->total) {
		target = h->total - 1;
	}
	uint64_t cum = 0;
	for (int i = 0; i < LAT_HIST_BUCKETS; i++) {
		cum += h->count[i];
		if (cum > target) {
			double lower = (double)lat_bucket_limits[i];
			double upper = (i < LAT_HIST_BUCKETS - 1)
				? (double)lat_bucket_limits[i + 1]
				: 20000.0;
			double prev = cum - (double)h->count[i];
			double frac = ((double)target - prev) / (double)h->count[i];
			return lower + (upper - lower) * frac;
		}
	}
	return 20000.0;
}

/** Zero-initialise a latency histogram. */
static inline void
latency_hist_reset(struct latency_hist *h)
{
	h->total = 0;
	memset(h->count, 0, sizeof(h->count));
}

/* Per-worker sampler -- cache-line-aligned to avoid false sharing
 * between the worker core (writer) and the master core (reader). */
struct entropy_sampler {
	/* producer fields -- worker lcore writes */
	struct entropy_5tuple samples[ENTROPY_RING_SIZE] __rte_cache_aligned;
	volatile uint64_t packet_count;   /* total pkts sent by this worker */
	volatile uint32_t write_idx;      /* next write position (wraps) */
	_Atomic uint32_t sample_interval; /* sample every Nth pkt */
	uint64_t sample_seed;             /* per-worker sampling phase */
	uint64_t prev_tsc;                /* previous pkt TSC for delta_tsc */
	uint8_t  pad_prod[28];            /* pad producer fields to 64B cache line */

	/* consumer fields -- master/core reads */
	uint32_t last_read_idx;           /* last drained position */
	uint64_t overwritten;             /* unread samples replaced by the writer */

	/* flow tracking -- drained by master along with samples */
	uint32_t flow_total_pkts;         /* total packets tracked for flow ratio */

	/* pad to next cache line, then flow hash set */
	uint8_t  pad_cons[48];

	/* HyperLogLog registers for cumulative distinct-flow estimation. */
	_Atomic uint8_t flow_hll[ENTROPY_FLOW_HLL_REGISTERS];

	/* latency histogram (aggregated by observer) */
	struct latency_hist lat_hist;
} __rte_cache_aligned;

/* helpers on the hot path */
/** Set the sampling phase.  A zero seed selects a fresh auto-seeded phase. */
static inline void
entropy_sampler_set_seed(struct entropy_sampler *s, uint64_t seed)
{
	s->sample_seed = seed ? seed : (rte_rdtsc() ^ (uint64_t)(uintptr_t)s);
}

/** Initialise a sampler (call once per worker).
 * @param seed  if non-zero, use as deterministic sample_seed;
 *              if zero, derive sample_seed from rdtsc. */
static inline void
entropy_sampler_init(struct entropy_sampler *s, uint32_t interval,
		     uint64_t seed)
{
	memset(s, 0, sizeof(*s));
	atomic_store_explicit(&s->sample_interval, interval,
			      memory_order_relaxed);   /* 0 = sampler disabled */
	entropy_sampler_set_seed(s, seed);
	latency_hist_reset(&s->lat_hist);
}

/** VXLAN header size: outer Eth + IP + UDP + VXLAN (8 bytes) */
#define ENTROPY_VXLAN_ENCAP_SIZE \
	(sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) \
	 + sizeof(struct rte_udp_hdr) + 8)

/** Build a compact flow key from a 5-tuple: XOR of all scalar fields. */
static inline uint32_t
entropy_make_flow_key(const struct entropy_5tuple *t)
{
	return (uint32_t)t->proto ^ t->src_ip ^ t->dst_ip
		^ (uint32_t)t->src_port ^ (uint32_t)t->dst_port;
}

/** Build a joint-entropy key: high byte of each field -> 32-bit.
 *  Compresses (proto, src_ip_hi, dst_ip_hi, src_port_lo) into one word. */
static inline uint32_t
entropy_make_joint_key(const struct entropy_5tuple *t)
{
	return ((uint32_t)t->proto << 24)
		| ((t->src_ip >> 24) & 0xff) << 16
		| ((t->dst_ip >> 24) & 0xff) << 8
		| (t->src_port & 0xff);
}

/** Extract 5-tuple + pkt_size + VXLAN info from a constructed mbuf.
 *  The caller already knows the BLESS_TYPE (`type`), which determines
 *  inner proto.  After bless_mbufs(), the mbuf may have had VXLAN
 *  outer headers prepended -- detect via ol_flags.
 *  When VXLAN: inner fields are read at offset ENTROPY_VXLAN_ENCAP_SIZE,
 *  outer fields from the data start, VNI from the VXLAN header. */
static inline void
entropy_extract_5tuple(const struct rte_mbuf *mbuf, int type,
		       struct entropy_5tuple *out)
{
	memset(out, 0, sizeof(*out));

	out->pkt_size = mbuf->pkt_len;

	/* Map type_idx -> IP protocol number */
	static const uint8_t builtin_proto[] = {255, 1, 6, 17, 132};
	if (type >= 0 && type < (int)(sizeof(builtin_proto))) {
		out->proto = builtin_proto[type];
	} else {
		out->proto = bless_pkt_ip_proto((unsigned int)type);
	}
	if (!out->proto) {
		return; /* unregistered type */
	}

	if (type == 0) {  /* ARP -- no IP / port at all */
		out->flow_key  = entropy_make_flow_key(out);
		out->joint_key = entropy_make_joint_key(out);
		return;
	}

	const struct rte_ether_hdr *eth =
		rte_pktmbuf_mtod(mbuf, const struct rte_ether_hdr *);

	int vxlan = !!(mbuf->ol_flags & RTE_MBUF_F_TX_TUNNEL_VXLAN);
	out->flags = vxlan;

	if (vxlan) {
		/* ---- outer (data start) ---- */
		const struct rte_ipv4_hdr *oip =
			(const struct rte_ipv4_hdr *)(eth + 1);
		const struct rte_udp_hdr *oudp =
			(const struct rte_udp_hdr *)(oip + 1);
		const uint8_t *vxlan_hdr = (const uint8_t *)(oudp + 1);

		out->outer_src_ip    = oip->src_addr;
		out->outer_dst_ip    = oip->dst_addr;
		out->outer_src_port  = oudp->src_port;
		out->vni = ((uint32_t)vxlan_hdr[4] << 16)
			 | ((uint32_t)vxlan_hdr[5] << 8)
			 |  vxlan_hdr[6];

		/* ---- inner (shifted by VXLAN encap) ---- */
		eth = (const struct rte_ether_hdr *)
			((const uint8_t *)eth + ENTROPY_VXLAN_ENCAP_SIZE);
	}

	/* inner IP header — handle both IPv4 and IPv6 */
	uint16_t etype = rte_be_to_cpu_16(eth->ether_type);
	if (etype == RTE_ETHER_TYPE_IPV6) {
		const struct rte_ipv6_hdr *ip6 =
			(const struct rte_ipv6_hdr *)(eth + 1);
		/* XOR-fold 128-bit src/dst into 32 bits */
		const uint32_t *s = (const uint32_t *)ip6->src_addr;
		const uint32_t *d = (const uint32_t *)ip6->dst_addr;
		out->src_ip = s[0] ^ s[1] ^ s[2] ^ s[3];
		out->dst_ip = d[0] ^ d[1] ^ d[2] ^ d[3];
		/* Determine next header / L4 offset */
		uint8_t nh = ip6->proto;
		if (nh == IPPROTO_TCP) {
			const struct rte_tcp_hdr *tcp =
				(const struct rte_tcp_hdr *)(ip6 + 1);
			out->src_port  = tcp->src_port;
			out->dst_port  = tcp->dst_port;
			out->tcp_flags = tcp->tcp_flags;
		} else if (nh == IPPROTO_UDP || nh == IPPROTO_SCTP) {
			const struct rte_udp_hdr *l4 =
				(const struct rte_udp_hdr *)(ip6 + 1);
			out->src_port = l4->src_port;
			out->dst_port = l4->dst_port;
		}
		/* else: no L4 ports (ICMPv6 etc.) */
	} else {
		/* IPv4 (or fallback) */
		const struct rte_ipv4_hdr *ip =
			(const struct rte_ipv4_hdr *)(eth + 1);
		out->src_ip = ip->src_addr;
		out->dst_ip = ip->dst_addr;

		if (type == 1) {  /* ICMP -- no L4 port */
			out->flow_key  = entropy_make_flow_key(out);
			out->joint_key = entropy_make_joint_key(out);
			return;
		}

		if (out->proto == IPPROTO_TCP) {
			const struct rte_tcp_hdr *tcp =
				(const struct rte_tcp_hdr *)(ip + 1);
			out->src_port  = tcp->src_port;
			out->dst_port  = tcp->dst_port;
			out->tcp_flags = tcp->tcp_flags;
		} else {  /* UDP, SCTP, or any L4 with ports at same offset */
			const struct rte_udp_hdr *l4 =
				(const struct rte_udp_hdr *)(ip + 1);
			out->src_port = l4->src_port;
			out->dst_port = l4->dst_port;
		}
	}

	out->flow_key  = entropy_make_flow_key(out);
	out->joint_key = entropy_make_joint_key(out);
}

/** Decide whether a packet at @p offset in the next TX burst is sampled.
 * The decision is stable when a partial burst is retried, but has no fixed
 * phase relationship with batch boundaries or protocol distribution cycles. */
static inline int
entropy_sampler_should_sample(const struct entropy_sampler *s, uint16_t offset)
{
	uint32_t interval = atomic_load_explicit(&s->sample_interval,
						 memory_order_relaxed);
	if (interval == 0) {
		return 0;
	}
	uint64_t seq = __atomic_load_n(&s->packet_count, __ATOMIC_RELAXED)
		+ offset;
	return entropy_sampler_select(seq, s->sample_seed, interval);
}

/** Record the interval between successful TX bursts. */
static inline uint64_t
entropy_sampler_tx_delta(struct entropy_sampler *s, uint64_t tx_tsc)
{
	uint64_t delta_tsc = s->prev_tsc ? tx_tsc - s->prev_tsc : 0;
	s->prev_tsc = tx_tsc;
	return delta_tsc;
}

/** Commit a sample known to have been accepted by the TX device. */
static inline void
entropy_sampler_commit(struct entropy_sampler *s,
		       const struct entropy_5tuple *t, uint64_t delta_tsc)
{

	uint32_t idx = __atomic_load_n(&s->write_idx, __ATOMIC_RELAXED);

	uint32_t slot = idx % ENTROPY_RING_SIZE;

	s->samples[slot] = *t;
	s->samples[slot].delta_tsc = delta_tsc;

	uint64_t h = entropy_sampler_mix64(
		((uint64_t)t->src_ip << 32) | t->dst_ip);
	h = entropy_sampler_mix64(h ^
		((uint64_t)t->src_port << 16) ^ t->dst_port);
	h = entropy_sampler_mix64(h ^ t->proto);
	uint32_t reg = (uint32_t)h & (ENTROPY_FLOW_HLL_REGISTERS - 1);
	uint8_t rank = entropy_flow_hll_rank(h);
	uint8_t old_rank = atomic_load_explicit(&s->flow_hll[reg],
		memory_order_relaxed);
	if (rank > old_rank) {
		atomic_store_explicit(&s->flow_hll[reg], rank,
				      memory_order_relaxed);
	}
	__atomic_fetch_add(&s->flow_total_pkts, 1, __ATOMIC_RELAXED);
	__atomic_store_n(&s->write_idx, idx + 1, __ATOMIC_RELEASE);
}

/** Advance the sampling sequence by packets accepted by TX. */
static inline void
entropy_sampler_advance(struct entropy_sampler *s, uint16_t sent)
{
	__atomic_fetch_add(&s->packet_count, sent, __ATOMIC_RELAXED);
}

/** Count distinct values in a sorted u32 array. */
static inline size_t
count_distinct_u32(const uint32_t *vals, size_t n)
{
	if (n == 0) {
		return 0;
	}
	size_t nd = 1;
	for (size_t i = 1; i < n; i++) {
		if (vals[i] != vals[i - 1]) {
			nd++;
		}
	}
	return nd;
}

/* latency histogram (log-scale buckets) */
/* global sampler registry */
/* Indexed by lcore_id; NULL = unused lcore.
 * Workers register during init; master drains during stats gen. */
extern struct entropy_sampler *entropy_samplers[RTE_MAX_LCORE];

#endif /* __ENTROPY_H__ */
