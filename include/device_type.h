#ifndef __DEVICE_TYPE_H__
#define __DEVICE_TYPE_H__

#include <string.h>

/* Device classes used to select queue and offload policies.  Both the
 * bus-probed Virtio PMD (net_virtio) and the userspace vdev frontend
 * (net_virtio_user) share the full-featured Virtio datapath in Bless. */
enum ethdev_type {
	ETHDEV_PHYSICAL,
	ETHDEV_PCAP,
	ETHDEV_VIRTIO,
	ETHDEV_RING,
	ETHDEV_NOT_SUPPORTED,
	ETHDEV_MAX,
};

/* Keep driver-name classification independent from DPDK so unit tests can
 * exercise the exact production logic without requiring an Ethdev instance. */
static inline enum ethdev_type
device_classify_driver_name(const char *drv)
{
	if (!drv) {
		return ETHDEV_MAX;
	}

	if (strcmp(drv, "net_pcap") == 0) {
		return ETHDEV_PCAP;
	}

	if (strcmp(drv, "net_virtio") == 0 ||
	    strcmp(drv, "net_virtio_user") == 0) {
		return ETHDEV_VIRTIO;
	}

	if (strcmp(drv, "net_ring") == 0 ||
	    strcmp(drv, "net_null") == 0) {
		return ETHDEV_RING;
	}

	if (strncmp(drv, "mlx5_pci", 8) == 0) {
		return ETHDEV_PHYSICAL;
	}

	return ETHDEV_NOT_SUPPORTED;
}

static inline int device_is_full_featured(enum ethdev_type type)
{
	return type == ETHDEV_PHYSICAL || type == ETHDEV_VIRTIO;
}

static inline int device_is_simple_vdev(enum ethdev_type type)
{
	return type == ETHDEV_PCAP || type == ETHDEV_RING;
}

#endif
