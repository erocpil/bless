#ifndef __WORKER_H__
#define __WORKER_H__

#include "define.h"

/* TODO rte_eth_hairpin_peer */
/* List of queues to be polled for a given lcore. 8< */
struct lcore_queue_conf {
	int enabled;
	int numa;
	uint16_t txl_id;
	uint16_t txp_id;
	uint16_t txq_id;
	unsigned n_rx_port;
	unsigned rx_port_list[MAX_RX_QUEUE_PER_LCORE];
} __rte_cache_aligned;
/* >8 End of list of queues to be polled for a given lcore. */

void worker_loop(void *conf);

#endif
