#ifndef __BLESS_CFG_H__
#define __BLESS_CFG_H__

#include <stddef.h>
#include <stdint.h>

/*
 * Bless Extension Config Interface
 *
 * Plugins can declare a YAML-to-C-struct field table so their config
 * is parsed generically by the framework -- no need to modify config.c.
 *
 * Usage (in proto_sctp.c):
 *
 *   struct sctp_cfg {
 *       uint16_t src[BLESS_CONFIG_MAX], dst[BLESS_CONFIG_MAX];
 *       uint16_t n_src, n_dst;
 *       int32_t  src_range, dst_range;
 *       char    *payload;
 *       uint16_t payload_len;
 *   };
 *
 *   static const struct bless_cfg_field sctp_fields[] = {
 *       { "src",    BLESS_CFG_PORT_RANGE,  offsetof(struct sctp_cfg, src),
 *         offsetof(struct sctp_cfg, n_src), PORT_MAX,
 *         offsetof(struct sctp_cfg, src_range) },
 *       { "dst",    BLESS_CFG_PORT_RANGE,  offsetof(struct sctp_cfg, dst),
 *         offsetof(struct sctp_cfg, n_dst), PORT_MAX,
 *         offsetof(struct sctp_cfg, dst_range) },
 *       { "payload", BLESS_CFG_STRING,     offsetof(struct sctp_cfg, payload),
 *         offsetof(struct sctp_cfg, payload_len), 0 },
 *       { NULL },
 *   };
 *
 *   static void __attribute__((constructor)) register_sctp_cfg(void) {
 *       static const struct bless_ext_cfg ext = {
 *           .name      = "sctp",
 *           .cfg_size  = sizeof(struct sctp_cfg),
 *           .fields    = sctp_fields,
 *           .yaml_path = "sctp",
 *           .free_cfg  = sctp_free,
 *           .clone_cfg = sctp_clone,
 *           .show_cfg  = sctp_show,
 *       };
 *       bless_register_cfg_parser(&ext);
 *   }
 */

/* Field type enum */enum bless_cfg_type {
    BLESS_CFG_UINT16,            /* scalar uint16 */
    BLESS_CFG_UINT32,            /* scalar uint32 */
    BLESS_CFG_IPV4,              /* scalar IPv4 address (+ optional range) */
    BLESS_CFG_PORT_RANGE,        /* scalar port with range syntax: 80+20 */
    BLESS_CFG_UINT16_ARRAY,      /* sequence of uint16 */
    BLESS_CFG_UINT32_ARRAY,      /* sequence of uint32 */
    BLESS_CFG_IPV4_ARRAY,        /* sequence of IPv4 addresses */
    BLESS_CFG_STRING,            /* payload string (malloc'd) */
    BLESS_CFG_TOGGLE,            /* boolean: "true"/"false" */
};

/** Callback for ext configs that expose port ranges (entropy calc) */
typedef void (*bless_port_range_fn)(const void *cfg,
    int32_t *src_range, uint16_t *n_src,
    int32_t *dst_range, uint16_t *n_dst);

/** Callback for custom YAML sub-tree parsing (beyond the fields[] table).
 *  Called after bless_parse_cfg_fields() finishes, with the ext's YAML
 *  sub-node.  Return 0 on success, -1 on error. */
typedef int (*bless_parse_cfg_fn)(void *node, void *cfg);

/* Field descriptor -- one per YAML key */struct bless_cfg_field {
    const char        *yaml_path;    /* relative YAML key, e.g. "src" */
    enum bless_cfg_type type;
    size_t             offset;       /* offset in plugin's config struct */
    size_t             offset_count; /* offset for count field (0 = no count) */
    uint16_t           limit;        /* max array elements (0 = no limit) */
    size_t             offset_range; /* offset for PORT_RANGE range field */
};

/* Extension config descriptor -- registered by plugins */struct bless_ext_cfg {
    const char                *name;       /* protocol name, e.g. "sctp" */
    size_t                     cfg_size;   /* sizeof(plugin's config struct) */
    const struct bless_cfg_field *fields;  /* null-terminated field array */
    const char                *yaml_path;  /* YAML sub-path under type.ipv4 */
    /* Lifecycle callbacks (all optional -- NULL = memcpy-based default) */
    void (*free_cfg)(void *cfg);
    int (*clone_cfg)(const void *src, void *dst);
    void (*show_cfg)(const void *cfg, int depth);
    bless_port_range_fn port_range;  /* expose port range for entropy calc */
    bless_parse_cfg_fn  parse_cfg;   /* custom YAML sub-tree parsing (optional) */
};

/* Registration (call from __attribute__((constructor))) */void bless_register_cfg_parser(const struct bless_ext_cfg *ext);

/* Lookup / iteration (used by config.c, cnode.c) */const struct bless_ext_cfg *bless_find_cfg_parser(const char *name);
void bless_foreach_cfg_parser(void (*fn)(const struct bless_ext_cfg *ext,
                                         void *ctx), void *ctx);

/* Generic field parsing (called from config.c) *//* Parse fields[] table against a YAML sub-node, populate cfg. */
int bless_parse_cfg_fields(const struct bless_cfg_field *fields,
                           void *cfg, void *yaml_node);

#endif /* __BLESS_CFG_H__ */
