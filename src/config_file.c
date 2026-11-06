#include "config_file.h"
#include "log.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

void
config_file_unmap_close(struct config_file_map *fm)
{
	if (!fm) {
		return;
	}
	if (fm->addr && fm->addr != MAP_FAILED) {
		munmap(fm->addr, fm->len);
	}
	if (fm->fd >= 0) {
		close(fm->fd);
	}
	fm->addr = NULL;
	fm->len = 0;
	fm->fd = -1;
}

int
config_file_map_open(struct config_file_map *cfm)
{
	struct stat st;
	if (!cfm || !cfm->name) {
		return -1;
	}
	cfm->fd = open(cfm->name, O_RDONLY);
	if (cfm->fd < 0) {
		return -1;
	}
	if (fstat(cfm->fd, &st) < 0 || !S_ISREG(st.st_mode)) {
		goto error;
	}
	if (st.st_size == 0) {
		cfm->addr = NULL;
		cfm->len = 0;
		return 0;
	}
	cfm->len = (size_t)st.st_size;
	cfm->addr = mmap(NULL, cfm->len, PROT_READ, MAP_PRIVATE, cfm->fd, 0);
	if (cfm->addr == MAP_FAILED) {
		goto error;
	}
	return 0;
error:
	config_file_unmap_close(cfm);
	return -1;
}

int
config_check_file_map(struct config_file_map *cfm)
{
	if (!cfm || !cfm->name) {
		return -1;
	}
	LOG_HINT("checking config file \"%s\"", cfm->name);
	if (config_file_map_open(cfm)) {
		LOG_ERR("config file \"%s\" open/mmap failed", cfm->name);
		return -1;
	}
	if (!cfm->len) {
		LOG_ERR("config file \"%s\" is empty", cfm->name);
		goto error;
	}

	size_t controls = 0;
	for (size_t i = 0; i < cfm->len; i++) {
		unsigned char c = ((const unsigned char *)cfm->addr)[i];
		if (!(isprint(c) || c == '\n' || c == '\r' || c == '\t')) {
			controls++;
		}
	}
	if ((double)controls / (double)cfm->len > 0.1) {
		LOG_ERR("config file \"%s\" is not a text file", cfm->name);
		goto error;
	}
	LOG_INFO("config file \"%s\" is readable text", cfm->name);
	return 0;
error:
	config_file_unmap_close(cfm);
	return -1;
}

Node *
config_init(struct config_file_map *cfm)
{
	if (!cfm || !cfm->addr || !cfm->len) {
		return NULL;
	}
	Node *root = config_yaml_parse_memory(cfm->addr, cfm->len);
	if (!root) {
		LOG_ERR("failed to parse YAML from %s",
			cfm->name ? cfm->name : "(null)");
	}
	return root;
}

int
config_exit(Node *root)
{
	config_yaml_free(root);
	return 0;
}
