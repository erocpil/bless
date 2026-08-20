#ifndef __BLESS_CONFIG_FILE_H__
#define __BLESS_CONFIG_FILE_H__

#include <stddef.h>

#include "config_yaml.h"

struct config_file_map {
	char *name;
	int fd;
	size_t len;
	void *addr;
};

int config_file_map_open(struct config_file_map *cfm);
void config_file_unmap_close(struct config_file_map *cfm);
int config_check_file_map(struct config_file_map *cfm);
Node *config_init(struct config_file_map *cfm);
int config_exit(Node *root);

#endif
