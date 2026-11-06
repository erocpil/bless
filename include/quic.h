#ifndef __QUIC_H__
#define __QUIC_H__

#include <stdint.h>
#include "bless.h"           /* mutation_func */

/**
 * QUIC v1 extension config.
 *
 * Stored in Cnode::ext[] when "quic" is present in YAML under
 * ether.type.ipv4 (or ipv6 in the future).
 */
struct quic_ext_cfg {
    uint8_t  dcid_len;     /* Destination Connection ID length (0-20) */
    uint8_t  scid_len;     /* Source Connection ID length (0-20) */
    uint32_t version;      /* QUIC version (0 = use QUIC v1 default) */
};

/** Look up the QUIC ext config from a Cnode, or NULL if not configured. */
struct Cnode;
struct quic_ext_cfg *quic_ext_find(struct Cnode *cnode);

/* Erroneous mutations (injectable via YAML ``erroneous.class.quic``) */
uint64_t mutation_quic_version(void **mbufs, unsigned int n, void *data);
uint64_t mutation_quic_cid_len(void **mbufs, unsigned int n, void *data);
uint64_t mutation_quic_token_len(void **mbufs, unsigned int n, void *data);

#define QUIC_MUTATORS_COUNT 3

#endif /* __QUIC_H__ */
