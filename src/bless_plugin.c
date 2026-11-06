#include "bless_plugin.h"
#include "bless_cfg.h"
#include "cnode.h"
#include "config_node.h"
#include "config_value.h"
#include "log.h"

/**
 * @file bless_plugin.c
 * @brief Extension registration tables and generic field parser.
 *
 * Manages two registries:
 *   - Packet-type constructors (bless_pkt_type)
 *   - Extension config parsers (bless_ext_cfg)
 *
 * New protocols register via __attribute__((constructor)) in their
 * proto_*.c file at static-init time.
 */

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <arpa/inet.h>

/* Packet-type registration tables (indexed by type_idx) */
static const struct bless_pkt_type *pkt_types[BLESS_MAX_TYPES];
static int32_t type_weights[BLESS_MAX_TYPES];  /* 0 = disabled */
static unsigned int next_ext_idx = BLESS_NUM_BUILTIN;

/* Config-parser registration table */
#define MAX_CFG_PARSERS 16

static const struct bless_ext_cfg *cfg_parsers[MAX_CFG_PARSERS];
static unsigned int n_cfg_parsers;

/* Packet-type API */
unsigned int bless_register_pkt_type(const struct bless_pkt_type *type)
{
    if (!type) {
        return UINT_MAX;
    }

    unsigned int idx = type->type_idx;

    /* Auto-assign for ext protocols */
    if (idx == BLESS_AUTO_IDX) {
        if (next_ext_idx >= BLESS_MAX_TYPES) {
            LOG_ERR("bless_register_pkt_type: too many types (max %u)", BLESS_MAX_TYPES);
            return UINT_MAX;
        }
        idx = next_ext_idx++;
    }

    if (idx >= BLESS_MAX_TYPES) {
        LOG_ERR("bless_register_pkt_type: type_idx %u out of range", idx);
        return UINT_MAX;
    }

    if (pkt_types[idx] != NULL && pkt_types[idx] != type) {
        LOG_ERR("bless_register_pkt_type: type_idx %u already registered (\"%s\")",
                idx, pkt_types[idx]->name);
        return UINT_MAX;
    }

    pkt_types[idx] = type;
    return idx;
}

void bless_set_type_weight(const char *name, int32_t weight)
{
    if (!name) {
        return;
    }
    for (unsigned int i = 0; i < BLESS_MAX_TYPES; i++) {
        if (pkt_types[i] && pkt_types[i]->name &&
            strcmp(pkt_types[i]->name, name) == 0) {
            type_weights[i] = weight;
            LOG_DEBUG("set weight %d for type \"%s\" (idx %u)", weight, name, i);
            return;
        }
    }
    LOG_WARN("bless_set_type_weight: type \"%s\" not registered", name);
}

int32_t bless_get_type_weight(const char *name)
{
    if (!name) {
        return 0;
    }
    for (unsigned int i = 0; i < BLESS_MAX_TYPES; i++) {
        if (pkt_types[i] && pkt_types[i]->name &&
            strcmp(pkt_types[i]->name, name) == 0) {
            return type_weights[i];
        }
    }
    return 0;
}

const struct bless_pkt_type *bless_find_pkt_type(const char *name)
{
    if (!name) {
        return NULL;
    }
    for (unsigned int i = 0; i < BLESS_MAX_TYPES; i++) {
        if (pkt_types[i] && pkt_types[i]->name &&
            strcmp(pkt_types[i]->name, name) == 0) {
            return pkt_types[i];
        }
    }
    return NULL;
}

void bless_foreach_pkt_type(void (*fn)(const struct bless_pkt_type *t,
                                       void *ctx), void *ctx)
{
    if (!fn) {
        return;
    }
    for (unsigned int i = 0; i < BLESS_MAX_TYPES; i++) {
        if (pkt_types[i]) {
            fn(pkt_types[i], ctx);
        }
    }
}

bless_pkt_ctor_t bless_get_ctor(unsigned int type_idx)
{
    if (type_idx >= BLESS_MAX_TYPES) {
        return NULL;
    }
    const struct bless_pkt_type *pt = pkt_types[type_idx];
    return pt ? pt->construct : NULL;
}

const char *bless_get_type_name(unsigned int type_idx)
{
    if (type_idx >= BLESS_MAX_TYPES || !pkt_types[type_idx]) {
        return "unknown";
    }
    return pkt_types[type_idx]->name;
}

/** Return the IP protocol number for a registered packet type.
 *  Returns 0 if type_idx is out of range or unregistered.
 *  Used by entropy extraction to tag samples for unknown protocols. */
uint8_t bless_pkt_ip_proto(unsigned int type_idx)
{
    if (type_idx >= BLESS_MAX_TYPES || !pkt_types[type_idx]) {
        return 0;
    }
    return pkt_types[type_idx]->ip_proto;
}

/* Config-parser API */
void bless_register_cfg_parser(const struct bless_ext_cfg *ext)
{
    if (!ext || !ext->name || !ext->fields || n_cfg_parsers >= MAX_CFG_PARSERS) {
        return;
    }
    cfg_parsers[n_cfg_parsers++] = ext;
}

const struct bless_ext_cfg *bless_find_cfg_parser(const char *name)
{
    if (!name) {
        return NULL;
    }
    for (unsigned int i = 0; i < n_cfg_parsers; i++) {
        if (cfg_parsers[i]->name &&
            strcmp(cfg_parsers[i]->name, name) == 0) {
            return cfg_parsers[i];
        }
    }
    return NULL;
}

void bless_foreach_cfg_parser(void (*fn)(const struct bless_ext_cfg *ext,
                                         void *ctx), void *ctx)
{
    if (!fn) {
        return;
    }
    for (unsigned int i = 0; i < n_cfg_parsers; i++)
        fn(cfg_parsers[i], ctx);
}

void bless_ext_aggregate_port_ranges(const Cnode *cnode,
                                     uint32_t *out_src, uint32_t *out_dst)
{
    if (!cnode || !out_src || !out_dst) {
        return;
    }
    for (uint8_t i = 0; i < cnode->n_ext; i++) {
        const struct bless_ext_cfg *desc = cnode->ext[i].desc;
        if (!desc || !desc->port_range || !cnode->ext[i].cfg) {
            continue;
        }
        int32_t sr = 0, dr = 0;
        uint16_t ns = 0, nd = 0;
        desc->port_range(cnode->ext[i].cfg, &sr, &ns, &dr, &nd);
        uint32_t n = (uint32_t)(sr > 0 ? sr : ns);
        if (n > *out_src) {
            *out_src = n;
        }
        n = (uint32_t)(dr > 0 ? dr : nd);
        if (n > *out_dst) {
            *out_dst = n;
        }
    }
}

/* Generic field parsing */
/* Parse "base+count" port-range syntax into {port[0], *range}. */
static int parse_port_range_field(const char *s, uint16_t *port, int32_t *range)
{
    if (!s || !*s) {
        return -1;
    }
    char *copy = strdup(s);
    if (!copy) {
        return -1;
    }

    int i = 0;
    while (copy[i] && copy[i] != '+') i++;

    if (copy[i] == '+') {
        copy[i] = '\0';
    if (config_value_uint16(copy, port) < 0 ||
        config_value_uint16(&copy[i + 1], (uint16_t*)range) < 0) {
            free(copy);
            return -1;
        }
    } else {
        if (config_value_uint16(copy, port) < 0) {
            free(copy);
            return -1;
        }
        *range = 0;
    }
    free(copy);
    return 0;
}

static int parse_ip_range_field(const char *s, uint32_t *ip, int64_t *range)
{
    if (!s || !*s) {
        return -1;
    }
    char *copy = strdup(s);
    if (!copy) {
        return -1;
    }

    int i = 0;
    while (copy[i] && copy[i] != '+') i++;

    if (copy[i] == '+') {
        copy[i] = '\0';
        char *endptr;
        errno = 0;
        unsigned long r = strtoul(&copy[i + 1], &endptr, 0);
        if (errno || *endptr) {
            free(copy);
            return -1;
        }
        *range = (int64_t)r;
    } else {
        *range = 0;
    }

    if (inet_pton(AF_INET, copy, ip) != 1) {
        free(copy);
        return -1;
    }
    free(copy);
    return 0;
}

int bless_parse_cfg_fields(const struct bless_cfg_field *fields,
                           void *cfg, void *yaml_node_v)
{
    if (!fields || !cfg || !yaml_node_v) {
        return -1;
    }

    Node *yaml_node = (Node *)yaml_node_v;

    for (const struct bless_cfg_field *f = fields; f->yaml_path; f++) {
        Node *val = find_by_path(yaml_node, f->yaml_path);
        if (!val) {
            continue; /* optional field -- skip */
        }

        void *field_ptr = (char *)cfg + f->offset;

        switch (f->type) {
        case BLESS_CFG_TOGGLE:
            if (val->type == NODE_SCALAR && val->value) {
                int on = (strcmp(val->value, "true") == 0 ||
                          strcmp(val->value, "TRUE") == 0);
                *(uint8_t *)field_ptr = (uint8_t)on;
            }
            break;

        case BLESS_CFG_UINT16:
            if (val->type == NODE_SCALAR && val->value) {
                config_value_uint16(val->value, (uint16_t *)field_ptr);
            }
            break;

        case BLESS_CFG_UINT32:
            if (val->type == NODE_SCALAR && val->value) {
                config_value_uint32(val->value, (uint32_t *)field_ptr);
            }
            break;

        case BLESS_CFG_IPV4: {
            if (val->type != NODE_SCALAR || !val->value) {
                break;
            }
            int64_t range = 0;
            uint32_t ip;
            if (parse_ip_range_field(val->value, &ip, &range) == 0) {
                *(uint32_t *)field_ptr = ip;
                if (f->offset_count) {
                    /* Store range at offset_count, not count (range mode) */
                    *(int64_t *)((char *)cfg + f->offset_count) = range;
                }
            }
            break;
        }

        case BLESS_CFG_PORT_RANGE: {
            if (val->type != NODE_SCALAR || !val->value) {
                break;
            }
            int32_t range = 0;
            uint16_t port;
            if (parse_port_range_field(val->value, &port, &range) == 0) {
                *(uint16_t *)field_ptr = port;
                if (f->offset_count) {
                    *(uint16_t *)((char *)cfg + f->offset_count) = 1;
                }
                if (f->offset_range) {
                    *(int32_t *)((char *)cfg + f->offset_range) = range;
                }
            }
            break;
        }

        case BLESS_CFG_UINT16_ARRAY:
            if (val->type == NODE_SEQUENCE) {
                uint16_t n = 0;
                for (Node *child = val->child; child && n < f->limit;
                     child = child->next, n++) {
                    config_value_uint16(child->value,
                        &((uint16_t *)field_ptr)[n]);
                }
                if (f->offset_count) {
                    *(uint16_t *)((char *)cfg + f->offset_count) = n;
                }
            } else if (val->type == NODE_SCALAR && val->value) {
                if (config_value_uint16(val->value, (uint16_t *)field_ptr) == 0) {
                    if (f->offset_count) {
                        *(uint16_t *)((char *)cfg + f->offset_count) = 1;
                    }
                }
            }
            break;

        case BLESS_CFG_UINT32_ARRAY:
            if (val->type == NODE_SEQUENCE) {
                uint16_t n = 0;
                for (Node *child = val->child; child && n < f->limit;
                     child = child->next, n++) {
                    config_value_uint32(child->value,
                        &((uint32_t *)field_ptr)[n]);
                }
                if (f->offset_count) {
                    *(uint16_t *)((char *)cfg + f->offset_count) = n;
                }
            } else if (val->type == NODE_SCALAR && val->value) {
                if (config_value_uint32(val->value, (uint32_t *)field_ptr) == 0) {
                    if (f->offset_count) {
                        *(uint16_t *)((char *)cfg + f->offset_count) = 1;
                    }
                }
            }
            break;

        case BLESS_CFG_IPV4_ARRAY:
            if (val->type == NODE_SEQUENCE) {
                uint16_t n = 0;
                for (Node *child = val->child; child && n < f->limit;
                     child = child->next, n++) {
                    inet_pton(AF_INET, child->value,
                              &((uint32_t *)field_ptr)[n]);
                }
                if (f->offset_count) {
                    *(uint16_t *)((char *)cfg + f->offset_count) = n;
                }
            } else if (val->type == NODE_SCALAR && val->value) {
                inet_pton(AF_INET, val->value, (uint32_t *)field_ptr);
                if (f->offset_count) {
                    *(uint16_t *)((char *)cfg + f->offset_count) = 1;
                }
            }
            break;

        case BLESS_CFG_STRING:
            if (val->value && strlen(val->value)) {
                char *payload = strdup(val->value);
                *(char **)field_ptr = payload;
                if (f->offset_count) {
                    *(uint16_t *)((char *)cfg + f->offset_count) =
                        (uint16_t)(strlen(payload) + 1);
                }
            }
            break;
        }
    }

    return 0;
}
