/* POSIX / C runtime */
#include <stdio.h>
#include <stdint.h>
// #include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

/* DPDK */
#include <rte_eal.h>
#include <rte_common.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

#include "device.h"
#include "log.h"

static char *ethdev_type_string[] = {
	"physical",
	"pcap",
	"virtio",
	"ring",
	"not_supported",
	"other",
};

char *device_get_string(uint16_t type)
{
	if (type >= ETHDEV_MAX) {
		return "other";
	}
	return ethdev_type_string[type];
}

inline uint16_t device_type_to_mask(enum ethdev_type type)
{
	return (1 << type);
}

enum ethdev_type device_get_ethdev_type(uint16_t portid)
{
	struct rte_eth_dev_info info;
	rte_eth_dev_info_get(portid, &info);

	const char *drv = info.driver_name;

	if (!drv) {
		return ETHDEV_MAX;
	}

	if (strcmp(drv, "net_pcap") == 0) {
		return ETHDEV_PCAP;
	}

	if (strcmp(drv, "net_virtio") == 0) {
		return ETHDEV_VIRTIO;
	}

	if (strcmp(drv, "net_ring") == 0) {
		return ETHDEV_RING;
	}

	/* 常见物理网卡 */
	if (strncmp(drv, "mlx5_pci", 8) == 0) {
		return ETHDEV_PHYSICAL;
	}

	return ETHDEV_NOT_SUPPORTED;
}

void device_show_info(uint16_t portid)
{
	struct rte_eth_dev_info dev_info;
	int ret = rte_eth_dev_info_get(portid, &dev_info);
	if (ret) {
		return;
	}

	LOG_INFO("dev info          %p", &dev_info);
	LOG_PATH("  port id         %u", portid);
	LOG_PATH("  driver name     %s", dev_info.driver_name);
	LOG_PATH("  if index        %u", dev_info.if_index);
	LOG_PATH("  mtu             [%d, %d]", dev_info.min_mtu, dev_info.max_mtu);
	LOG_PATH("  max rx queues   %u", dev_info.max_rx_queues);
	LOG_PATH("  max tx queues   %u", dev_info.max_tx_queues);
	LOG_PATH("  nb rx queues    %u", dev_info.nb_rx_queues);
	LOG_PATH("  nb tx queues    %u", dev_info.nb_tx_queues);
	LOG_PATH("  tx offload capa %lx", dev_info.tx_offload_capa);
}
