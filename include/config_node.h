#ifndef __BLESS_CONFIG_NODE_H__
#define __BLESS_CONFIG_NODE_H__

#include "config_yaml.h"

/** Search a Node tree using a dotted path with optional sequence indexes. */
Node *find_by_path(Node *root, const char *path);

#endif
