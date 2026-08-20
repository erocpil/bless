#ifndef __BLESS_SCTP_H__
#define __BLESS_SCTP_H__

#include <stdint.h>
#include <stddef.h>
#include "cnode.h"

/* IPPROTO_SCTP (132) -- may not be defined in all environments */
#ifndef IPPROTO_SCTP
#define IPPROTO_SCTP 132
#endif

/* SCTP extension config (stored in Cnode::ext[]) */
struct sctp_ext_cfg {
	uint16_t src[BLESS_CONFIG_MAX];
	uint16_t dst[BLESS_CONFIG_MAX];
	uint16_t n_src;
	uint16_t n_dst;
	int32_t  src_range;
	int32_t  dst_range;
	char    *payload;
	uint16_t payload_len;
};

/** Find the SCTP ext config from a Cnode. Returns NULL if not configured. */
static inline struct sctp_ext_cfg *sctp_ext_find(Cnode *cnode)
{
	for (uint8_t i = 0; i < cnode->n_ext; i++) {
		if (cnode->ext[i].desc && cnode->ext[i].cfg &&
		    __builtin_strcmp(cnode->ext[i].desc->name, "sctp") == 0) {
			return (struct sctp_ext_cfg *)cnode->ext[i].cfg;
		}
	}
	return NULL;
}

#endif /* __BLESS_SCTP_H__ */
