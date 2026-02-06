#ifndef __WORKER_H__
#define __WORKER_H__

#include "base.h"

struct worker {
	int mode;
	struct rte_mbuf **mbufs;
	struct rte_mbuf **rx_mbufs;

	/* thread name */
	struct bless_conf conf;
	struct base_core_view cv;
	Cnode cnode;
	cpu_set_t cpuset;
	char name[256];
};

void worker_loop(void *conf);
void worker_main_loop(void *conf);
void ws_user_func(void *user, void *data, size_t size);
void worker_show(struct worker *w);

#endif
