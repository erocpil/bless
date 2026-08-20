#include "config_file.h"
#include "log.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int g_log_format_json;
static const theme_config test_theme = {
	.name = "test",
	.green = "", .yellow = "", .red = "", .blue = "",
	.purple = "", .grey = "",
};
const theme_config *g_current_theme = &test_theme;

static void
write_all(int fd, const char *text)
{
	size_t len = strlen(text);
	assert(write(fd, text, len) == (ssize_t)len);
}

int
main(void)
{
	char path[] = "/tmp/bless-config-file-XXXXXX";
	int fd = mkstemp(path);
	assert(fd >= 0);
	write_all(fd, "system:\n  daemonize: false\n");
	assert(close(fd) == 0);

	struct config_file_map map = {
		.name = path,
		.fd = -1,
	};
	assert(config_check_file_map(&map) == 0);
	assert(map.fd >= 0 && map.addr && map.len > 0);
	Node *root = config_init(&map);
	assert(root != NULL);
	assert(config_exit(root) == 0);
	config_file_unmap_close(&map);
	assert(map.fd == -1 && map.addr == NULL && map.len == 0);
	assert(unlink(path) == 0);

	puts("config file ownership: PASS");
	return 0;
}
