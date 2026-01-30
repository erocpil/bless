#ifndef __WORKER_H__
#define __WORKER_H__

#include "define.h"

/* TODO rte_eth_hairpin_peer */
/* List of queues to be polled for a given lcore. 8< */
struct lcore_queue_conf {
	int enabled;
	int numa;
	int type;
	uint16_t txl_id;
	uint16_t txp_id;
	uint16_t txq_id;
	unsigned n_rx_port;
	unsigned rx_port_list[MAX_RX_QUEUE_PER_LCORE];
} __attribute__((__aligned__(sizeof(char)))); /* DO NOT TOUCH ATTR!!! */
/* >8 End of list of queues to be polled for a given lcore. */

void worker_loop(void *conf);
void worker_main_loop(void *conf);
void ws_user_func(void *user, void *data, size_t size);

#endif
