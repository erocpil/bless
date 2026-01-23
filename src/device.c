#include <sys/types.h>   /* for ssize_t */
#include <unistd.h>     /* for ssize_t, POSIX */
#include <string.h>     /* for strnlen */

/* POSIX / C runtime */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
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

enum ethdev_type device_get_ethdev_type(uint16_t portid)
{
	struct rte_eth_dev_info info;
	rte_eth_dev_info_get(portid, &info);

	const char *drv = info.driver_name;

	if (!drv) {
		return ETHDEV_OTHER;
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

void device_print(uint16_t portid)
{
	struct rte_eth_dev_info dev_info;
	int ret = rte_eth_dev_info_get(portid, &dev_info);
	if (ret) {
		return;
	}
	printf("port %u: driver=%s, max_txq=%u, offload=0x%lx\n",
			portid,
			dev_info.driver_name,
			dev_info.max_tx_queues,
			dev_info.tx_offload_capa);

}
