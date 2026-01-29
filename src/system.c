#include "system.h"
#include "log.h"
#include <string.h>

void system_set_defaults(struct system_cfg *cfg)
{
	if (!cfg) {
		printf("Null system config.\n");
		return;
	}
	memset(cfg, 0, sizeof(struct system_cfg));
}

static void system_print_cpuset(const cpu_set_t *set, int max_cpus)
{
	int first = 1;
	int start = -1;

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

void system_dump_status(struct system_status *sysstat)
{
	printf("\nSystem status %p:\n", sysstat);
	if (!sysstat) {
		return;
	}
	printf("  ppid    %u\n", sysstat->ppid);
	printf("  pid     %u\n", sysstat->pid);
	printf("  cpuset  ");
	system_print_cpuset(&sysstat->cpuset, CPU_SETSIZE);
}

void system_show_cfg(struct system_cfg *cfg)
{
	if (!cfg) {
		return;
	}
	LOG_HINT("system cfg %p", cfg);
	LOG_PATH("  daemonize %d", cfg->daemonize);
	server_show_options_cfg_format(&cfg->srvcfg, "  ");
}
