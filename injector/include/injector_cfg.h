#ifndef __INJECTOR_CFG_H__
#define __INJECTOR_CFG_H__

#include <stddef.h>
#include <stdint.h>

/*
 * Injector Extension Config Interface
 *
 * Plugins can declare a YAML-to-C-struct field table so their config
 * is parsed generically by the framework -- no need to modify config.c.
 *
 * Usage (in proto_example.c):
 *
 *   struct example_cfg {
 *       uint16_t src[INJECTOR_CONFIG_MAX], dst[INJECTOR_CONFIG_MAX];
 *       uint16_t n_src, n_dst;
 *       int32_t  src_range, dst_range;
 *       char    *payload;
 *       uint16_t payload_len;
 *   };
 *
 *   static const struct injector_cfg_field example_fields[] = {
 *       { "src",    INJECTOR_CFG_PORT_RANGE,  offsetof(struct example_cfg, src),
 *         offsetof(struct example_cfg, n_src), PORT_MAX },
 *       { "dst",    INJECTOR_CFG_PORT_RANGE,  offsetof(struct example_cfg, dst),
 *         offsetof(struct example_cfg, n_dst), PORT_MAX },
 *       { "payload", INJECTOR_CFG_STRING,     offsetof(struct example_cfg, payload),
 *         offsetof(struct example_cfg, payload_len), 0 },
 *       { NULL },
 *   };
 *
 *   static void __attribute__((constructor)) register_example_cfg(void) {
 *       static const struct injector_ext_cfg ext = {
 *           .name      = "example",
 *           .cfg_size  = sizeof(struct example_cfg),
 *           .fields    = example_fields,
 *           .yaml_path = "example",
 *           .free_cfg  = example_free,
 *           .clone_cfg = example_clone,
 *           .show_cfg  = example_show,
 *       };
 *       injector_register_cfg_parser(&ext);
 *   }
 */

/* Field type enum */enum injector_cfg_type {
    INJECTOR_CFG_UINT16,            /* scalar uint16 */
    INJECTOR_CFG_UINT32,            /* scalar uint32 */
    INJECTOR_CFG_IPV4,              /* scalar IPv4 address (+ optional range) */
    INJECTOR_CFG_PORT_RANGE,        /* scalar port with range syntax: 80+20 */
    INJECTOR_CFG_UINT16_ARRAY,      /* sequence of uint16 */
    INJECTOR_CFG_UINT32_ARRAY,      /* sequence of uint32 */
    INJECTOR_CFG_IPV4_ARRAY,        /* sequence of IPv4 addresses */
    INJECTOR_CFG_STRING,            /* payload string (malloc'd) */
    INJECTOR_CFG_TOGGLE,            /* boolean: "true"/"false" */
};

/** Callback for custom YAML sub-tree parsing (beyond the fields[] table).
 *  Called after injector_parse_cfg_fields() finishes, with the ext's YAML
 *  sub-node.  Return 0 on success, -1 on error. */
typedef int (*injector_parse_cfg_fn)(void *node, void *cfg);

/* Field descriptor -- one per YAML key */struct injector_cfg_field {
    const char        *yaml_path;    /* relative YAML key, e.g. "src" */
    enum injector_cfg_type type;
    size_t             offset;       /* offset in plugin's config struct */
    size_t             offset_count; /* offset for count field (0 = no count) */
    uint16_t           limit;        /* max array elements (0 = no limit) */
};

/* Extension config descriptor -- registered by plugins */struct injector_ext_cfg {
    const char                *name;       /* protocol name, e.g. "example" */
    size_t                     cfg_size;   /* sizeof(plugin's config struct) */
    const struct injector_cfg_field *fields;  /* null-terminated field array */
    const char                *yaml_path;  /* YAML sub-path under type.ipv4 */
    /* Lifecycle callbacks (all optional -- NULL = memcpy-based default) */
    void (*free_cfg)(void *cfg);
    void (*clone_cfg)(const void *src, void *dst);
    void (*show_cfg)(const void *cfg, int depth);
    injector_parse_cfg_fn  parse_cfg;   /* custom YAML sub-tree parsing (optional) */
};

/* Registration (call from __attribute__((constructor))) */void injector_register_cfg_parser(const struct injector_ext_cfg *ext);

/* Lookup / iteration (used by config.c, cnode.c) */const struct injector_ext_cfg *injector_find_cfg_parser(const char *name);
void injector_foreach_cfg_parser(void (*fn)(const struct injector_ext_cfg *ext,
                                         void *ctx), void *ctx);

/* Generic field parsing (called from config.c) *//* Parse fields[] table against a YAML sub-node, populate cfg. */
int injector_parse_cfg_fields(const struct injector_cfg_field *fields,
                           void *cfg, void *yaml_node);

#endif /* __INJECTOR_CFG_H__ */
