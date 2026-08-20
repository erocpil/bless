#include "args.h"
#include "log.h"

#include <string.h>
#include <stdlib.h>

/**
 * @file args.c
 * @brief CLI override injection for YAML config.
 *
 * Saves argv tokens after the config file path and a trailing "--"
 * separator, then re-injects them into a second getopt_long() pass
 * so that command-line overrides take precedence over YAML values.
 */

/* module-level storage */
static int  saved_argc = 0;
static char **saved_argv = NULL;

/* public interface */
void args_save_cli_overrides(int argc, char **argv)
{
	saved_argc = 0;
	saved_argv = NULL;

	/* Need at least: prog config.yaml [--] [override ...] */
	if (argc < 3) {
		return;
	}

	/* Special sub-commands: these exit early in init_config() */
	if (!strcmp(argv[1], "version") ||
	    !strcmp(argv[1], "--version") ||
	    !strcmp(argv[1], "config") ||
	    !strcmp(argv[1], "help")) {
		return;
	}

	/* argv[0] = prog, argv[1] = config.yaml */
	int off = 2;

	/* Optional DPDK-style -- separator */
	if (off < argc && !strcmp(argv[off], "--")) {
		off++;
	}

	saved_argc = argc - off;
	saved_argv = argv + off;

	if (saved_argc > 0) {
		LOG_HINT("CLI overrides saved: %d arg(s)%s",
			 saved_argc, off > 2 ? " (with --)" : "");
	}
}

void args_inject_cli_overrides(int *argcp, char ***argvp)
{
	if (saved_argc == 0) {
		return;
	}

	int old_argc = *argcp;
	int new_argc = old_argc + saved_argc;

	char **new_argv = realloc(*argvp,
			(size_t)(new_argc + 1) * sizeof(char *));
	if (!new_argv) {
		LOG_ERR("realloc(%zu) for CLI overrides failed",
			(size_t)(new_argc + 1) * sizeof(char *));
		exit(EXIT_FAILURE);
	}

	for (int i = 0; i < saved_argc; i++) {
		new_argv[old_argc + i] = strdup(saved_argv[i]);
		if (!new_argv[old_argc + i]) {
			LOG_ERR("strdup() for CLI override %d failed", i);
			exit(EXIT_FAILURE);
		}
	}
	new_argv[new_argc] = NULL;

	LOG_TRACE("CLI overrides injected: %d arg(s)", saved_argc);

	*argcp = new_argc;
	*argvp = new_argv;
}
