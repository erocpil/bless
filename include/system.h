#ifndef __SYSTEM_H__
#define __SYSTEM_H__
#include <stdint.h>
#include <stddef.h>
#include <unistd.h> /* for sleep() */
#include "server.h"

struct system_cfg {
	uint8_t daemonize;
	struct server_options_cfg srvcfg;
};

void system_set_defaults(struct system_cfg *cfg);

#endif
