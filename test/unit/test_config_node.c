#include "config_node.h"

#include <assert.h>
#include <stdio.h>

int
main(void)
{
	Node second = { .key = NULL, .value = "b", .type = NODE_SCALAR };
	Node first = { .key = NULL, .value = "a", .type = NODE_SCALAR,
		.next = &second };
	Node list = { .key = "ports", .type = NODE_SEQUENCE, .child = &first };
	Node leaf = { .key = "name", .value = "bless", .type = NODE_SCALAR,
		.next = &list };
	Node root = { .type = NODE_MAPPING, .child = &leaf };

	assert(find_by_path(&root, "name") == &leaf);
	assert(find_by_path(&root, "ports[0]") == &first);
	assert(find_by_path(&root, "ports[1]") == &second);
	assert(find_by_path(&root, "ports[2]") == NULL);
	assert(find_by_path(&root, "name[0]") == NULL);
	assert(find_by_path(&root, "missing") == NULL);
	assert(find_by_path(NULL, "name") == NULL);
	assert(find_by_path(&root, NULL) == NULL);
	puts("config node lookup: PASS");
	return 0;
}
