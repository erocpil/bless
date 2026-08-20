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

/* Full-featured NIC: supports multi-queue, descriptor negotiation,
 * and full TX/RX offload configuration.  PHYSICAL and VIRTIO PMDs
 * both provide this; simple virtual devices (PCAP, RING) do not. */
static inline int device_is_full_featured(enum ethdev_type type)
{
	return type == ETHDEV_PHYSICAL || type == ETHDEV_VIRTIO;
}

/* Simple virtual device: limited queue model (0 or 1 RX, 1 TX).
 * Used for PCAP replay and ring-based testing. */
static inline int device_is_simple_vdev(enum ethdev_type type)
{
	return type == ETHDEV_PCAP || type == ETHDEV_RING;
}

enum ethdev_type device_get_ethdev_type(uint16_t portid);
void device_show_info(uint16_t portid);
char *device_get_string(uint16_t type);
uint16_t device_type_to_mask(enum ethdev_type type);

#endif
