#ifndef __BASE_H__
#define __BASE_H__

#include "bless.h"
#include "system.h"
#include <stdint.h>

#ifndef USE_VERSION_H
#define BL_VERSION "1.0"
#define GIT_COMMIT "N/A"
#define GIT_BRANCH "N/A"
#define BUILD_TIME "N/A"
#define BUILD_HOST "N/A"
#define BUILD_TYPE "N/A"
#else
#include "version.h"
#endif

#define MAX_TIMER_PERIOD 86400 /* 1 day max */

struct base_core_view {
	uint8_t enabled;
	uint8_t numa;
	uint8_t role; /* core role */
	uint16_t core; /* core */
	uint16_t port; /* port */
	uint16_t type; /* port type */
	uint16_t txq; /* tx queue */
	uint16_t rxq; /* rx queue */
	uint16_t n_rx_port;
	uint16_t rx_port_list[MAX_RX_QUEUE_PER_LCORE]; /* unused */
} __attribute__((__aligned__(sizeof(char)))); /* DO NOT TOUCH ATTR!!! */

/* base topology: socket, numa, role, core, port, queue 8< */
struct base_topo {
	uint16_t n_socket;
	uint16_t n_numa;
	uint16_t n_core; /* ROLE_RTE worker only */
	uint16_t n_enabled_core; /* also worker */
	uint16_t n_port;
	uint16_t n_enabled_port;
	uint32_t port_mask;
	uint32_t main_core; /* yes, main core's here */
	struct base_core_view *cv;
};

struct base {
	struct base_topo topo;
	uint16_t nb_rxd;
	uint16_t nb_txd;
	/* only ETHDEV_PHYSICAL and ETHDEV_PCAP */
	uint32_t dev_mode_mask;
	atomic_int *g_state;
	int mac_updating;
	int promiscuous_on;
	// uint32_t enabled_port_mask;
	uint32_t enabled_lcores;
	unsigned int rxtxq_per_port;
	struct system_status sysstat;
	struct rte_mempool rx_pktmbuf_pool;
	struct lcore_queue_conf *lcore_queue_conf;

	int argc;
	char **argv;
	uint16_t args_jump;

	struct system *system;
	struct config *config;
	struct bless_conf *bconf;

	pthread_barrier_t barrier;

	int return_value;
};
/* >8 End base topology. */

void base_show();
void base_show_topo(struct base_topo *topo);
void base_show_core_view(struct base_core_view *view);

#endif
