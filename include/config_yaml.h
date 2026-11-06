#ifndef BLESS_CONFIG_YAML_H
#define BLESS_CONFIG_YAML_H

#include <stddef.h>

typedef enum {
	NODE_SCALAR,
	NODE_MAPPING,
	NODE_SEQUENCE
} NodeType;

typedef struct Node {
	char *key;
	char *value;
	NodeType type;
	struct Node *child;
	struct Node *next;
} Node;

/*
 * Parse one or more YAML documents from memory into the production Node tree.
 * Multiple document roots are linked through Node::next.  Returns NULL for an
 * empty stream, malformed input, or allocation failure.
 */
Node *config_yaml_parse_memory(const unsigned char *data, size_t size);

/* Free a tree returned by config_yaml_parse_memory(). */
void config_yaml_free(Node *node);

#endif
