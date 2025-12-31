#include <string.h>
#include "system.h"

void system_set_defaults(struct system_cfg *cfg)
{
	if (!cfg) {
		return;
	}
	memset(cfg, 0, sizeof(struct system_cfg));
}
