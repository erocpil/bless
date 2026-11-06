/* SPDX-License-Identifier: BSD-3-Clause */
#include "bless_dpdk_runtime.h"

#include <errno.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_lcore.h>
#include <rte_spinlock.h>
#include <stdlib.h>

struct br_pool_entry {
	struct rte_mempool *pool;
	struct br_pool_entry *next;
};

static struct br_pool_entry *owned_pools;
static rte_spinlock_t pool_lock = RTE_SPINLOCK_INITIALIZER;

int br_runtime_eal_init(int argc, char **argv)
{
	return rte_eal_init(argc, argv);
}

int br_runtime_eal_cleanup(void)
{
	return rte_eal_cleanup();
}

int br_runtime_port_configure(uint16_t port_id,
		const struct br_runtime_port_config *config,
		uint16_t *actual_rx_descriptors,
		uint16_t *actual_tx_descriptors)
{
	struct rte_eth_dev_info info;
	struct rte_eth_conf port_conf;
	struct rte_eth_rxconf rxconf;
	struct rte_eth_txconf txconf;
	uint16_t rx_desc, tx_desc;
	int ret;

	if (!config || !config->port_conf ||
	    (!config->rx_queues && !config->tx_queues) ||
	    (config->rx_queues && !config->rx_pool))
		return -EINVAL;
	ret = rte_eth_dev_info_get(port_id, &info);
	if (ret < 0)
		return ret;
	if (config->rx_queues > info.max_rx_queues ||
	    config->tx_queues > info.max_tx_queues)
		return -ENOSPC;

	port_conf = *config->port_conf;
	ret = rte_eth_dev_configure(port_id, config->rx_queues,
			config->tx_queues, &port_conf);
	if (ret < 0)
		return ret;

	rx_desc = config->rx_descriptors;
	tx_desc = config->tx_descriptors;
	if (config->adjust_rx_descriptors || config->adjust_tx_descriptors) {
		ret = rte_eth_dev_adjust_nb_rx_tx_desc(port_id,
				config->adjust_rx_descriptors ? &rx_desc : NULL,
				config->adjust_tx_descriptors ? &tx_desc : NULL);
		if (ret < 0)
			return ret;
	}

	rxconf = config->rx_queue_conf ? *config->rx_queue_conf :
		info.default_rxconf;
	txconf = config->tx_queue_conf ? *config->tx_queue_conf :
		info.default_txconf;
	int socket_id = rte_eth_dev_socket_id(port_id);
	for (uint16_t queue = 0; queue < config->rx_queues; queue++) {
		ret = rte_eth_rx_queue_setup(port_id, queue, rx_desc, socket_id,
				&rxconf, config->rx_pool);
		if (ret < 0)
			return ret;
	}
	for (uint16_t queue = 0; queue < config->tx_queues; queue++) {
		ret = rte_eth_tx_queue_setup(port_id, queue, tx_desc, socket_id,
				&txconf);
		if (ret < 0)
			return ret;
	}
	if (actual_rx_descriptors)
		*actual_rx_descriptors = rx_desc;
	if (actual_tx_descriptors)
		*actual_tx_descriptors = tx_desc;
	return 0;
}

int br_runtime_port_start(uint16_t port_id)
{
	return rte_eth_dev_start(port_id);
}

int br_runtime_ports_stop(uint32_t enabled_port_mask)
{
	uint16_t port_id;
	int first_error = 0;

	RTE_ETH_FOREACH_DEV(port_id) {
		if (port_id >= 32 || !(enabled_port_mask & (1u << port_id)))
			continue;
		int ret = rte_eth_dev_stop(port_id);
		if (ret < 0 && !first_error)
			first_error = ret;
		ret = rte_eth_dev_close(port_id);
		if (ret < 0 && !first_error)
			first_error = ret;
	}
	return first_error;
}

struct rte_mempool *br_runtime_pktmbuf_pool_create(const char *name,
		uint32_t count, uint32_t cache_size, uint16_t private_size,
		uint16_t data_room_size, int socket_id)
{
	if (!name || !count || !data_room_size) {
		rte_errno = EINVAL;
		return NULL;
	}
	struct rte_mempool *pool = rte_pktmbuf_pool_create(name, count,
			cache_size, private_size, data_room_size, socket_id);
	if (!pool)
		return NULL;
	struct br_pool_entry *entry = malloc(sizeof(*entry));
	if (!entry) {
		rte_mempool_free(pool);
		rte_errno = ENOMEM;
		return NULL;
	}
	entry->pool = pool;
	rte_spinlock_lock(&pool_lock);
	entry->next = owned_pools;
	owned_pools = entry;
	rte_spinlock_unlock(&pool_lock);
	return pool;
}

struct rte_mempool *br_runtime_tx_pool_create(const char *name,
		uint32_t count, int socket_id)
{
	return br_runtime_pktmbuf_pool_create(name, count, 0, 0,
			RTE_MBUF_DEFAULT_BUF_SIZE, socket_id);
}

void br_runtime_mempools_release(void)
{
	rte_spinlock_lock(&pool_lock);
	struct br_pool_entry *entries = owned_pools;
	owned_pools = NULL;
	rte_spinlock_unlock(&pool_lock);
	while (entries) {
		struct br_pool_entry *next = entries->next;
		rte_mempool_free(entries->pool);
		free(entries);
		entries = next;
	}
}

uint16_t br_runtime_tx_burst(uint16_t port_id, uint16_t queue_id,
		struct rte_mbuf **packets, uint16_t count)
{
	return rte_eth_tx_burst(port_id, queue_id, packets, count);
}
