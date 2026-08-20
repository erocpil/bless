/**
 * flow_entropy.c -- Per-flow entropy computation for handshake mode.
 *
 * Drains per-worker flow samplers, builds empirical histograms over
 * flow 5-tuples, lifetimes, and event types, then fills the corresponding
 * fields in struct stats_snapshot.
 *
 * Prefix: flow_entropy_*
 */
#include "worker.h"
#include "server.h"
#include "entropy.h"  /* shannon_from_sorted, cmp_u32, cmp_u64 */

#include <stdlib.h>  /* qsort */

#define FE_MAX  FLOW_RING_SIZE

static double cached_flow_5tuple;
static double cached_flow_event;
static double cached_flow_lifetime;

void
flow_entropy_compute(struct stats_snapshot *s)
{
	uint32_t five_tuples[FE_MAX];  /* flow key for 5-tuple entropy */
	uint32_t events[FE_MAX];       /* event types */
	uint64_t lifetimes[FE_MAX];    /* lifetime TSC deltas */
	size_t n5t = 0, nev = 0, nlt = 0;

	unsigned main_lc = rte_get_main_lcore();

	for (unsigned lc = 0; lc < RTE_MAX_LCORE; lc++) {
		struct handshake_ctx *h = handshake_ctxs[lc];
		if (!h || lc == main_lc) {
			continue;
		}

		struct flow_sampler *fs = &h->flow_sampler;
		uint32_t wi = __atomic_load_n(&fs->write_idx,
					       __ATOMIC_ACQUIRE);
		uint32_t last = fs->last_read_idx;
		uint32_t nnew = wi - last;

		if (nnew == 0 || nnew > FLOW_RING_SIZE * 2) {
			fs->last_read_idx = wi;
			continue;
		}

		for (uint32_t i = 0; i < nnew && i < FLOW_RING_SIZE; i++) {
			const struct flow_sample *t =
				&fs->ring[(last + i) % FLOW_RING_SIZE];

			/* 5-tuple flow key: XOR for distinct flow counting */
			if (n5t < FE_MAX) {
				five_tuples[n5t++] = (uint32_t)t->src_ip
					^ (uint32_t)t->dst_ip
					^ (uint32_t)t->src_port
					^ (uint32_t)t->dst_port;
			}

			/* event type histogram */
			if (nev < FE_MAX) {
				events[nev++] = t->event;
			}

			/* lifetime (skip CREATED / ESTABLISHED events which
			 * have lifetime_tsc = 0 or meaningless values) */
			if (t->event >= 2 && nlt < FE_MAX) {
				lifetimes[nlt++] = t->lifetime_tsc >> 12;
			}
		}

		fs->last_read_idx = wi;
	}

	/* sort each dimension */
	if (n5t > 1) {
		qsort(five_tuples, n5t, sizeof(uint32_t), cmp_u32);
	}
	if (nev > 1) {
		qsort(events, nev, sizeof(uint32_t), cmp_u32);
	}
	if (nlt > 1) {
		qsort(lifetimes, nlt, sizeof(uint64_t), cmp_u64);
	}

	/* Shannon entropy */
	if (n5t > 1) {
		cached_flow_5tuple = shannon_from_sorted(five_tuples, n5t,
							 sizeof(uint32_t), cmp_u32, NULL);
	}
	if (nev > 1) {
		cached_flow_event = shannon_from_sorted(events, nev,
							sizeof(uint32_t), cmp_u32, NULL);
	}
	if (nlt > 1) {
		cached_flow_lifetime = shannon_from_sorted(lifetimes, nlt,
							   sizeof(uint64_t), cmp_u64, NULL);
	}
	s->flow_entropy_5tuple = cached_flow_5tuple;
	s->flow_entropy_event = cached_flow_event;
	s->flow_entropy_lifetime = cached_flow_lifetime;
	s->flow_count = (double)(n5t);
	s->flow_lifetime_count = (double)nlt;
	s->flow_event_count = (double)nev;
}
