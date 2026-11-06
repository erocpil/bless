#ifndef __WORKER_H__
#define __WORKER_H__

#include <rte_lcore.h>

#include "base.h"
#include "config.h"   /* dist_ratio */
#include "cnode.h"    /* Cnode */
#include "token_bucket.h"

struct worker {
	struct rte_mbuf **mbufs;

	/* thread name */
	struct injector_conf conf;
	struct base_core_view cv;
	Cnode cnode;
	cpu_set_t cpuset;
	char name[256];

	/* per-worker token buckets (PPS / BPS) */
	struct token_bucket pps_bucket;
	struct token_bucket bps_bucket;
};

void worker_loop(void *conf);

void worker_main_loop(void *conf);

void worker_show(const struct worker *w);

enum { CHECK_CONTINUE = 0, CHECK_EXIT = 1 };
int worker_check_state(atomic_int *state, struct base_core_view *cv,
		       int64_t num);

#endif
