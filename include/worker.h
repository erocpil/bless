#ifndef __WORKER_H__
#define __WORKER_H__

#include "base.h"


struct worker {
	struct rte_mbuf **mbufs;
	struct rte_mbuf **rx_mbufs;
	int mode;

	uint16_t core;
	uint16_t port;
	uint16_t type;
	uint16_t txq;
	uint16_t rxq;

	/* thread name */
	struct bless_conf conf;
	struct base_core_view cv;
	Cnode cnode;
	char name[256];
};

void worker_loop(void *conf);
void worker_main_loop(void *conf);
void ws_user_func(void *user, void *data, size_t size);

#endif
