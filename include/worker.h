#ifndef __WORKER_H__
#define __WORKER_H__

#include "define.h"

struct worker_core_view {
	uint8_t enabled;
	uint8_t socket;
	uint8_t numa;
	uint8_t role; /* core role */
	uint16_t core; /* core */
	uint16_t port; /* port */
	uint16_t type; /* port type */
	uint16_t txq; /* tx queue */
	uint16_t rxq; /* rx queue */
	uint16_t n_rx_port;
	uint16_t rx_port_list[MAX_RX_QUEUE_PER_LCORE];
};

struct worker {
	int mode;
	char *thead_name;
	struct rte_mbuf **mbufs;
	struct rte_mbuf **rx_mbufs;
	struct worker_core_view cv;
};

void worker_loop(void *conf);
void worker_main_loop(void *conf);
void ws_user_func(void *user, void *data, size_t size);

#endif
