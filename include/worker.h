#ifndef __WORKER_H__
#define __WORKER_H__

#include "bless.h"

/* List of queues to be polled for a given lcore. 8< */
struct lcore_queue_conf {
	unsigned n_rx_port;
	unsigned rx_port_list[MAX_RX_QUEUE_PER_LCORE];
} __rte_cache_aligned;
/* >8 End of list of queues to be polled for a given lcore. */

void worker_loop_txonly(void *conf);

#endif
