#ifndef __DEVICE_H__
#define __DEVICE_H__

#include "rte_ethdev.h"
#include <stdint.h>

enum ethdev_type {
	ETHDEV_PHYSICAL,
	ETHDEV_PCAP,
	ETHDEV_VIRTIO,
	ETHDEV_RING,
	ETHDEV_NOT_SUPPORTED,
	ETHDEV_MAX,
};

enum ethdev_type_mask {
	ETHDEV_PHYSICAL_MASK = 1 << ETHDEV_PHYSICAL,
	ETHDEV_PCAP_MASK = 1 << ETHDEV_PCAP,
	ETHDEV_VIRTIO_MASK = 1 << ETHDEV_VIRTIO,
	ETHDEV_RING_MASK = 1 << RTE_ETH_DEV_REMOVED,
	ETHDEV_NOT_SUPPORTED_MASK = 1 << ETHDEV_NOT_SUPPORTED,
	ETHDEV_MAX_MASK = 1 << ETHDEV_MAX,
};

enum ethdev_type device_get_ethdev_type(uint16_t portid);
void device_show_info(uint16_t portid);
char *device_get_string(uint16_t type);
uint16_t device_type_to_mask(enum ethdev_type type);

#endif
