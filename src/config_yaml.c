#include "config_yaml.h"

#include <stdlib.h>
#include <string.h>
#include <yaml.h>

static Node *node_new(const char *value, NodeType type)
{
	Node *node = calloc(1, sizeof(*node));
	if (!node) {
		return NULL;
	}

	node->type = type;
	if (value) {
		node->value = strdup(value);
		if (!node->value) {
			free(node);
			return NULL;
		}
	}

	return node;
}

void config_yaml_free(Node *node)
{
	if (!node) {
		return;
	}

	config_yaml_free(node->child);
	config_yaml_free(node->next);
	free(node->key);
	free(node->value);
	free(node);
}

static int node_add_child(Node *parent, Node *child)
{
	if (!parent || !child) {
		return -1;
	}

	if (!parent->child) {
		parent->child = child;
		return 0;
	}

	Node *tail = parent->child;
	while (tail->next)
		tail = tail->next;
	tail->next = child;
	return 0;
}

static Node *parse_node(yaml_parser_t *parser, int *ok);

static Node *parse_mapping(yaml_parser_t *parser, int *ok)
{
	Node *mapping = node_new(NULL, NODE_MAPPING);
	if (!mapping) {
		*ok = 0;
		return NULL;
	}

	for (;;) {
		yaml_event_t event;
		if (!yaml_parser_parse(parser, &event)) {
			*ok = 0;
			goto fail;
		}

		if (event.type == YAML_MAPPING_END_EVENT) {
			yaml_event_delete(&event);
			return mapping;
		}

		if (event.type != YAML_SCALAR_EVENT) {
			yaml_event_delete(&event);
			continue;
		}

		char *key = strdup((const char *)event.data.scalar.value);
		yaml_event_delete(&event);
		if (!key) {
			*ok = 0;
			goto fail;
		}

		Node *value = parse_node(parser, ok);
		if (!*ok || !value) {
			free(key);
			goto fail;
		}
		value->key = key;
		if (node_add_child(mapping, value) != 0) {
			config_yaml_free(value);
			*ok = 0;
			goto fail;
		}
	}

fail:
	config_yaml_free(mapping);
	return NULL;
}

static Node *parse_sequence(yaml_parser_t *parser, int *ok)
{
	Node *sequence = node_new(NULL, NODE_SEQUENCE);
	if (!sequence) {
		*ok = 0;
		return NULL;
	}

	for (;;) {
		yaml_event_t event;
		if (!yaml_parser_parse(parser, &event)) {
			*ok = 0;
			goto fail;
		}

		if (event.type == YAML_SEQUENCE_END_EVENT) {
			yaml_event_delete(&event);
			return sequence;
		}

		Node *child = NULL;
		switch (event.type) {
		case YAML_SCALAR_EVENT:
			child = node_new((const char *)event.data.scalar.value,
				NODE_SCALAR);
			yaml_event_delete(&event);
			if (!child) {
				*ok = 0;
			}
			break;
		case YAML_MAPPING_START_EVENT:
			yaml_event_delete(&event);
			child = parse_mapping(parser, ok);
			break;
		case YAML_SEQUENCE_START_EVENT:
			yaml_event_delete(&event);
			child = parse_sequence(parser, ok);
			break;
		default:
			yaml_event_delete(&event);
			continue;
		}

		if (!*ok || !child) {
			goto fail;
		}
		if (node_add_child(sequence, child) != 0) {
			config_yaml_free(child);
			*ok = 0;
			goto fail;
		}
	}

fail:
	config_yaml_free(sequence);
	return NULL;
}

static Node *parse_node(yaml_parser_t *parser, int *ok)
{
	yaml_event_t event;
	if (!yaml_parser_parse(parser, &event)) {
		*ok = 0;
		return NULL;
	}

	Node *node = NULL;
	switch (event.type) {
	case YAML_SCALAR_EVENT:
		node = node_new((const char *)event.data.scalar.value, NODE_SCALAR);
		yaml_event_delete(&event);
		if (!node) {
			*ok = 0;
		}
		return node;
	case YAML_MAPPING_START_EVENT:
		yaml_event_delete(&event);
		return parse_mapping(parser, ok);
	case YAML_SEQUENCE_START_EVENT:
		yaml_event_delete(&event);
		return parse_sequence(parser, ok);
	default:
		yaml_event_delete(&event);
		*ok = 0;
		return NULL;
	}
}

Node *config_yaml_parse_memory(const unsigned char *data, size_t size)
{
	if (!data || size == 0) {
		return NULL;
	}

	yaml_parser_t parser;
	if (!yaml_parser_initialize(&parser)) {
		return NULL;
	}
	yaml_parser_set_input_string(&parser, data, size);

	Node *root = NULL;
	Node **tail = &root;
	int ok = 1;

	for (;;) {
		yaml_event_t event;
		if (!yaml_parser_parse(&parser, &event)) {
			ok = 0;
			break;
		}

		if (event.type == YAML_STREAM_END_EVENT) {
			yaml_event_delete(&event);
			break;
		}

		if (event.type != YAML_DOCUMENT_START_EVENT) {
			yaml_event_delete(&event);
			continue;
		}

		yaml_event_delete(&event);
		Node *document = parse_node(&parser, &ok);
		if (!ok || !document) {
			break;
		}

		*tail = document;
		while (*tail)
			tail = &(*tail)->next;
	}

	yaml_parser_delete(&parser);
	if (!ok) {
		config_yaml_free(root);
		return NULL;
	}
	return root;
}
