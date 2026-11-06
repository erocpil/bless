/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef BLESS_DPDK_RUNTIME_H
#define BLESS_DPDK_RUNTIME_H

#include <stdint.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

int br_runtime_eal_init(int argc, char **argv);
int br_runtime_eal_cleanup(void);

struct br_runtime_port_config {
	uint16_t rx_queues;
	uint16_t tx_queues;
	uint16_t rx_descriptors;
	uint16_t tx_descriptors;
	uint8_t adjust_rx_descriptors;
	uint8_t adjust_tx_descriptors;
	const struct rte_eth_conf *port_conf;
	const struct rte_eth_rxconf *rx_queue_conf;
	const struct rte_eth_txconf *tx_queue_conf;
	struct rte_mempool *rx_pool;
};

/* Configure the port and queues without starting it, allowing application
 * policy (e.g. ptype settings) to be applied before br_runtime_port_start(). */
int br_runtime_port_configure(uint16_t port_id,
		const struct br_runtime_port_config *config,
		uint16_t *actual_rx_descriptors,
		uint16_t *actual_tx_descriptors);
int br_runtime_port_start(uint16_t port_id);
int br_runtime_ports_stop(uint32_t enabled_port_mask);

struct rte_mempool *br_runtime_pktmbuf_pool_create(const char *name,
		uint32_t count, uint32_t cache_size, uint16_t private_size,
		uint16_t data_room_size, int socket_id);
struct rte_mempool *br_runtime_tx_pool_create(const char *name,
		uint32_t count, int socket_id);
void br_runtime_mempools_release(void);

/* PMD owns [0, returned); caller owns and frees the unsent suffix. */
uint16_t br_runtime_tx_burst(uint16_t port_id, uint16_t queue_id,
		struct rte_mbuf **packets, uint16_t count);

#endif
