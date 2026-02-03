#include "system.h"
#include "log.h"
#include "server.h"
#include <string.h>

void system_set_defaults(struct system_cfg *cfg)
{
	if (!cfg) {
		printf("Null system config.\n");
		return;
	}
	memset(cfg, 0, sizeof(struct system_cfg));
}

void system_show_cpuset(const cpu_set_t *set, int max_cpus)
{
	int first = 1;
	int start = -1;

	LOG_META_NNL("  cpuset  ");
	for (int cpu = 0; cpu <= max_cpus; cpu++) {
		int is_set = (cpu < max_cpus) && CPU_ISSET(cpu, set);

		if (is_set) {
			if (start < 0) {
				start = cpu;
			}
		} else {
			if (start >= 0) {
				if (!first) {
					printf(",");
				}
				if (start == cpu - 1) {
					printf("%d", start);
				} else {
					printf("%d-%d", start, cpu - 1);
				}

				first = 0;
				start = -1;
			}
		}
	}

	if (first) {
		printf("(empty)");
	}
	printf("\n");
}

void system_show_status(struct system_status *sysstat)
{
	printf("\n");
	LOG_INFO("System status %p   ", sysstat);
	if (!sysstat) {
		return;
	}
	LOG_PATH("  ppid    %u", sysstat->ppid);
	LOG_PATH("  pid     %u", sysstat->pid);
	system_show_cpuset(&sysstat->cpuset, CPU_SETSIZE);
}

void system_show(struct system *sys)
{
	if (!sys) {
		return;
	}
	LOG_HINT("system %p", sys);
	LOG_PATH("  daemonize %d", sys->cfg.daemonize);
	LOG_PATH("  theme     %s", sys->cfg.theme);
	server_show_format(&sys->cfg.server, "   ");
}
