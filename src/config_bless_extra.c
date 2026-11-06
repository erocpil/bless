#include "config_bless_internal.h"

#include "bless_cfg.h"
#include "config_node.h"
#include "erroneous.h"
#include "log.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

int
config_parse_bless_erroneous(Node *bless_node, Cnode *cnode)
{
	cnode->erroneous.ratio = 0;
	Node *node = find_by_path(bless_node, "erroneous.ratio");
	if (node && node->value && *node->value) {
		char *end;
		errno = 0;
		long ratio = strtol(node->value, &end, 0);
		if (!errno && end != node->value && !*end && ratio > 0 &&
		    ratio <= UINT16_MAX) {
			cnode->erroneous.ratio = (uint16_t)ratio;
		}
	}
	if (!cnode->erroneous.ratio) {
		LOG_INFO("erroneous disabled due to no valid ratio");
		return 1;
	}

	node = find_by_path(bless_node, "erroneous.class");
	if (!node || !node->child) {
		cnode->erroneous.ratio = 0;
		LOG_INFO("erroneous disabled due to no class");
		return 1;
	}

	int mutations = 0;
	for (Node *entry = node->child; entry; entry = entry->next) {
		if (cnode->erroneous.n_clas >= BLESS_ERRNONEOUS_CLASS_TYPE_MAX) {
			LOG_WARN("erroneous: at most %d classes, rest ignored",
				BLESS_ERRNONEOUS_CLASS_TYPE_MAX);
			break;
		}
		struct ec_clas *clas =
			&cnode->erroneous.clas[cnode->erroneous.n_clas];
		clas->name = strdup(entry->key);
		if (!clas->name) {
			return -1;
		}
		cnode->erroneous.n_clas++;

		for (Node *item = entry->child; item; item = item->next) {
			if (clas->n_type >= BLESS_CONFIG_MAX) {
				LOG_WARN("erroneous.class.%s: at most %d mutations, rest ignored",
					clas->name, BLESS_CONFIG_MAX);
				break;
			}
			clas->type[clas->n_type] = strdup(item->value);
			if (!clas->type[clas->n_type]) {
				return -1;
			}
			clas->n_type++;
			mutations++;
		}
	}
	cnode->erroneous.n_mutation = mutations;
	if (!mutations) {
		return 1;
	}

	mutation_func *func = malloc(sizeof(*func) * (size_t)mutations);
	if (!func) {
		return -1;
	}
	int pos = 0;
	for (int i = 0; i < cnode->erroneous.n_clas; i++) {
		struct ec_clas *clas = &cnode->erroneous.clas[i];
		for (int j = 0; j < clas->n_type; j++) {
			mutation_func found =
				find_mutation_func(clas->name, clas->type[j]);
			if (!found) {
				LOG_WARN("no mutation `%s:%s'", clas->name, clas->type[j]);
				free(func);
				return -1;
			}
			func[pos++] = found;
		}
	}
	cnode->erroneous.func = func;
	return mutations;
}

struct plugin_parse_context {
	Node *ether_node;
	Cnode *cnode;
	int failed;
};

static void
parse_one_plugin(const struct bless_ext_cfg *ext, void *opaque)
{
	struct plugin_parse_context *ctx = opaque;
	if (ctx->failed) {
		return;
	}
	Node *node = find_by_path(ctx->ether_node, ext->yaml_path);
	if (!node) {
		return;
	}
	if (ctx->cnode->n_ext >= BLESS_PLUGIN_MAX) {
		LOG_ERR("too many protocol extension configurations");
		ctx->failed = 1;
		return;
	}

	void *cfg = calloc(1, ext->cfg_size);
	if (!cfg) {
		ctx->failed = 1;
		return;
	}
	if (bless_parse_cfg_fields(ext->fields, cfg, node) < 0 ||
			(ext->parse_cfg && ext->parse_cfg(node, cfg) < 0)) {
		if (ext->free_cfg) {
			ext->free_cfg(cfg);
		}
		free(cfg);
		ctx->failed = 1;
		return;
	}

	uint8_t slot = ctx->cnode->n_ext++;
	ctx->cnode->ext[slot].desc = ext;
	ctx->cnode->ext[slot].cfg = cfg;
}

int
config_parse_bless_plugins(Node *ether_node, Cnode *cnode)
{
	struct plugin_parse_context ctx = {
		.ether_node = ether_node,
		.cnode = cnode,
	};
	bless_foreach_cfg_parser(parse_one_plugin, &ctx);
	return ctx.failed ? -1 : 0;
}
