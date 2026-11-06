#ifndef BLESS_STATS_GUARD_H
#define BLESS_STATS_GUARD_H

#include <stdatomic.h>

/* Two-slot publication guard shared by the production stats path and TSan. */
struct stats_guard {
	_Atomic int active;
	_Atomic int readers[2];
};

int stats_guard_active(const struct stats_guard *guard);
void stats_guard_publish(struct stats_guard *guard, int index);
int stats_guard_acquire(struct stats_guard *guard);
void stats_guard_release(struct stats_guard *guard, int index);
int stats_guard_writable(const struct stats_guard *guard, int index);

#endif
