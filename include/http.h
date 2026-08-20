#ifndef __BLESS_HTTP_H__
#define __BLESS_HTTP_H__

#include <stdint.h>
#include <stddef.h>
#include "cnode.h"

/* HTTP extension config (stored in Cnode::ext[]) */
struct http_ext_cfg {
	uint16_t src[BLESS_CONFIG_MAX];
	uint16_t dst[BLESS_CONFIG_MAX];
	uint16_t n_src;
	uint16_t n_dst;
	int32_t  src_range;
	int32_t  dst_range;
	/* methods to mix (e.g. GET, POST, HEAD) -- comma-separated string parsed at runtime */
	char     methods[128];
	/* URI paths to rotate (e.g. /,/api,/index.html) -- comma-separated */
	char     paths[256];
	/* Host header values to rotate (e.g. example.com,api.example.org) */
	char     hosts[256];
};

/** Find the HTTP ext config from a Cnode. */
static inline struct http_ext_cfg *http_ext_find(Cnode *cnode)
{
	for (uint8_t i = 0; i < cnode->n_ext; i++) {
		if (cnode->ext[i].desc && cnode->ext[i].cfg &&
		    __builtin_strcmp(cnode->ext[i].desc->name, "http") == 0) {
			return (struct http_ext_cfg *)cnode->ext[i].cfg;
		}
	}
	return NULL;
}

/** Number of HTTP mutation functions. */
#define HTTP_MUTATORS_COUNT 4

#endif /* __BLESS_HTTP_H__ */
