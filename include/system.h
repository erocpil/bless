#ifndef __SYSTEM_H__
#define __SYSTEM_H__

#include <sched.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <unistd.h>
#include "server.h"

struct system_cfg {
	uint8_t daemonize;
	char theme[16];
	struct server server;
};

struct system_status {
	pid_t pid;
	pid_t ppid;
	cpu_set_t cpuset;
};

struct system {
	struct system_cfg cfg;
	struct system_status status;
};

void system_set_defaults(struct system_cfg *cfg);
void system_show(struct system *sys);
void system_show_status(struct system_status *sysstat);
void system_show_cpuset(const cpu_set_t *set, int max_cpus);

#endif
