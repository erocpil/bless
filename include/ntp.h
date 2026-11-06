#ifndef __BLESS_NTP_H__
#define __BLESS_NTP_H__

#include <stdint.h>
#include <stddef.h>
#include "cnode.h"

/* NTP header (RFC 5905), 48 bytes */
struct ntp_hdr {
	uint8_t  li_vn_mode;  /* leap indicator(2) + version(3) + mode(3) */
	uint8_t  stratum;
	int8_t   poll;
	int8_t   precision;
	uint32_t root_delay;
	uint32_t root_dispersion;
	uint32_t ref_id;
	uint64_t ref_ts;       /* reference timestamp */
	uint64_t orig_ts;      /* originate timestamp */
	uint64_t recv_ts;      /* receive timestamp */
	uint64_t xmit_ts;      /* transmit timestamp */
} __attribute__((packed));

#define NTP_HDR_SIZE  48

/* NTP extension config (stored in Cnode::ext[]) */
struct ntp_ext_cfg {
	uint16_t src[BLESS_CONFIG_MAX];
	uint16_t dst[BLESS_CONFIG_MAX];
	uint16_t n_src;
	uint16_t n_dst;
	int32_t  src_range;
	int32_t  dst_range;
	uint8_t  modes[8];     /* NTP modes to mix (3=client, 4=server, etc.) */
	uint8_t  n_modes;
	uint8_t  versions[4];  /* NTP versions (3,4) */
	uint8_t  n_versions;
};

/** Find the NTP ext config from a Cnode. */
static inline struct ntp_ext_cfg *ntp_ext_find(Cnode *cnode)
{
	for (uint8_t i = 0; i < cnode->n_ext; i++) {
		if (cnode->ext[i].desc && cnode->ext[i].cfg &&
		    __builtin_strcmp(cnode->ext[i].desc->name, "ntp") == 0) {
			return (struct ntp_ext_cfg *)cnode->ext[i].cfg;
		}
	}
	return NULL;
}

/** Number of NTP mutation functions. */
#define NTP_MUTATORS_COUNT 4

#endif /* __BLESS_NTP_H__ */
