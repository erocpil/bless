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

/* Full-featured NIC: supports multi-queue, descriptor negotiation,
 * and full TX offload configuration.  PHYSICAL and VIRTIO PMDs
 * both provide this; simple virtual devices (PCAP, RING) do not. */
static inline int device_is_full_featured(enum ethdev_type type)
{
	return type == ETHDEV_PHYSICAL || type == ETHDEV_VIRTIO;
}

/* Simple virtual device: one TX queue.
 * Used for PCAP output and ring-based testing. */
static inline int device_is_simple_vdev(enum ethdev_type type)
{
	return type == ETHDEV_PCAP || type == ETHDEV_RING;
}

enum ethdev_type device_get_ethdev_type(uint16_t portid);
void device_show_info(uint16_t portid);
char *device_get_string(uint16_t type);
static inline int device_port_enabled(uint32_t mask, uint16_t port)
{
    return port < 32 && (mask & (1u << port));
}

#endif
