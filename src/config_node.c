#include "config_node.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Node *
find_by_path(Node *root, const char *path)
{
	if (!root || !path || !*path) {
		return NULL;
	}

	char *copy = strdup(path);
	if (!copy) {
		return NULL;
	}
	char *save = NULL;
	char *token = strtok_r(copy, ".", &save);
	Node *cur = root;

	while (token && cur) {
		char key[128];
		int idx = -1;
		if (sscanf(token, "%127[^[][%d]", key, &idx) < 1) {
			cur = NULL;
			break;
		}

		Node *child = cur->child;
		while (child && (!child->key || strcmp(child->key, key)))
			child = child->next;
		cur = child;
		if (cur && idx >= 0) {
			if (cur->type != NODE_SEQUENCE) {
				cur = NULL;
				break;
			}
			Node *elem = cur->child;
			for (int i = 0; elem && i < idx; i++)
				elem = elem->next;
			cur = elem;
		}
		token = strtok_r(NULL, ".", &save);
	}

	free(copy);
	return cur;
}
