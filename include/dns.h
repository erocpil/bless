#ifndef __BLESS_DNS_H__
#define __BLESS_DNS_H__

#include <stdint.h>
#include <stddef.h>
#include "cnode.h"

/* DNS header (RFC 1035 section 4.1.1), 12 bytes fixed */
struct dns_hdr {
	uint16_t id;          /* transaction ID (random for entropy) */
	uint16_t flags;       /* 0x0100 = standard query, recursion desired */
	uint16_t qdcount;     /* question count (typically 1) */
	uint16_t ancount;     /* answer count (0 for query) */
	uint16_t nscount;     /* authority count (0) */
	uint16_t arcount;     /* additional count (0) */
} __attribute__((packed));

/* DNS question suffix (after variable-length QNAME):
 *   QTYPE  (2 bytes, 1=A, 28=AAAA, etc.)
 *   QCLASS (2 bytes, 1=IN) */
struct dns_question_tail {
	uint16_t qtype;
	uint16_t qclass;
} __attribute__((packed));

#define DNS_HDR_SIZE      12
#define DNS_QUESTION_TAIL 4  /* QTYPE + QCLASS */

/* DNS extension config (stored in Cnode::ext[]) */
struct dns_ext_cfg {
	uint16_t src[BLESS_CONFIG_MAX];
	uint16_t dst[BLESS_CONFIG_MAX];
	uint16_t n_src;
	uint16_t n_dst;
	int32_t  src_range;
	int32_t  dst_range;
	/* query names (dot-separated labels, each null-terminated) */
	char    *names[BLESS_CONFIG_MAX];
	uint16_t n_names;
	/* query types (1=A, 28=AAAA, 15=MX, 2=NS, etc.) */
	uint16_t qtypes[BLESS_CONFIG_MAX];
	uint16_t n_qtypes;
};

/** Find the DNS ext config from a Cnode. Returns NULL if not configured. */
static inline struct dns_ext_cfg *dns_ext_find(Cnode *cnode)
{
	for (uint8_t i = 0; i < cnode->n_ext; i++) {
		if (cnode->ext[i].desc && cnode->ext[i].cfg &&
		    __builtin_strcmp(cnode->ext[i].desc->name, "dns") == 0) {
			return (struct dns_ext_cfg *)cnode->ext[i].cfg;
		}
	}
	return NULL;
}

/** Number of DNS mutation functions. */
#define DNS_MUTATORS_COUNT 5

#endif /* __BLESS_DNS_H__ */
