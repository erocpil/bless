#ifndef __SYSTEM_H__
#define __SYSTEM_H__

#define _GNU_SOURCE             /* See feature_test_macros(7) */
#include <sched.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <unistd.h>
#include "server.h"

struct system_cfg {
	uint8_t daemonize;
	struct server_options_cfg srvcfg;
};

struct system_status {
	pid_t pid;
	pid_t ppid;
	cpu_set_t cpuset;
};

void system_dump_status(struct system_status *sysstat);

void system_set_defaults(struct system_cfg *cfg);

#endif
