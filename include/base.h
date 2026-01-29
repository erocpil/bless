#ifndef __BASE_H__
#define __BASE_H__

#include "bless.h"
#include "system.h"
#include <stdint.h>

struct base_core_view {
	int enabled;
	char socket;
	char numa;
	char role;
	int type;
	uint16_t core; /* core */
	uint16_t port; /* port */
	uint16_t txq; /* tx queue */
	uint16_t rxq; /* rx queue */
	unsigned n_rx_port;
	unsigned rx_port_list[MAX_RX_QUEUE_PER_LCORE];
} __attribute__((__aligned__(sizeof(char)))); /* DO NOT TOUCH ATTR!!! */

/* base topology: socket, numa, role, core, port, queue 8< */
struct base_topo {
	uint16_t n_socket;
	uint16_t n_numa;
	uint16_t n_core; /* ROLE_RTE only */
	uint16_t n_enabled_core;
	uint16_t n_port;
	uint16_t n_enabled_port;
	uint32_t port_mask;
	uint32_t main_core;
	struct base_core_view *cv;
};

struct base {
	struct base_topo topo;
	uint16_t nb_rxd;
	uint16_t nb_txd;
	/* only ETHDEV_PHYSICAL and ETHDEV_PCAP */
	int mode;
	atomic_int *g_state;
	int mac_updating;
	int promiscuous_on;
	// uint32_t enabled_port_mask;
	uint32_t enabled_lcores;
	unsigned int rxtxq_per_port;
	uint64_t timer_period; /* default period is 10 seconds */
	struct system_status sysstat;
	struct rte_mempool rx_pktmbuf_pool;
	struct lcore_queue_conf *lcore_queue_conf;

	int argc;
	char **argv;

	struct system *system;
	struct config *config;
	struct bless_conf *bconf;

	pthread_barrier_t barrier;

	int return_value;
};
/* >8 End base topology. */

#endif
