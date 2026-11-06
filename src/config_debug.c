#include "config.h"
#include "log.h"

#include <stdio.h>

typedef void (*NodeVisitor)(Node *node, int depth, void *userdata);

static void
traverse(Node *node, int depth, NodeVisitor pre, NodeVisitor post,
		void *userdata)
{
	for (; node; node = node->next) {
		if (pre) {
			pre(node, depth, userdata);
		}
		if (node->child) {
			traverse(node->child, depth + 1, pre, post, userdata);
		}
		if (post) {
			post(node, depth, userdata);
		}
	}
}

static void
print_pre(Node *node, int depth, void *userdata)
{
	(void)userdata;
	for (int i = 0; i < depth; i++)
		printf("  ");
	if (node->key) {
		printf("%s: ", node->key);
	}
	if (node->type == NODE_SCALAR) {
		printf("%s\n", node->value ? node->value : "null");
	} else if (node->type == NODE_MAPPING) {
		printf("{\n");
	} else if (node->type == NODE_SEQUENCE) {
		printf("[\n");
	}
}

static void
print_post(Node *node, int depth, void *userdata)
{
	(void)userdata;
	if (node->type != NODE_MAPPING && node->type != NODE_SEQUENCE) {
		return;
	}
	for (int i = 0; i < depth; i++)
		printf("  ");
	printf(node->type == NODE_MAPPING ? "}\n" : "]\n");
}

void
config_show(struct config *cfg)
{
	LOG_HINT("config       %p", cfg);
	LOG_HINT("  file map   %p", &cfg->cfm);
	LOG_PATH("    name     %s", cfg->cfm.name);
	LOG_PATH("    fd       %d", cfg->cfm.fd);
	LOG_PATH("    len      %zu", cfg->cfm.len);
	LOG_PATH("    addr     %p", cfg->cfm.addr);
	LOG_HINT("  root       %p", cfg->root);
	LOG_HINT("  cnode      %p", cfg->cnode);
}

void
config_show_root(struct config *cfg)
{
	traverse(cfg->root, 0, print_pre, print_post, NULL);
}
