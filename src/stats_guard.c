#include "stats_guard.h"

int stats_guard_active(const struct stats_guard *guard)
{
	return atomic_load_explicit(&guard->active, memory_order_acquire);
}

void stats_guard_publish(struct stats_guard *guard, int index)
{
	atomic_store_explicit(&guard->active, index, memory_order_release);
}

int stats_guard_acquire(struct stats_guard *guard)
{
	for (;;) {
		int index = stats_guard_active(guard);

		atomic_fetch_add_explicit(&guard->readers[index], 1,
					  memory_order_acq_rel);
		if (index == stats_guard_active(guard)) {
			return index;
		}

		atomic_fetch_sub_explicit(&guard->readers[index], 1,
					  memory_order_release);
	}
}

void stats_guard_release(struct stats_guard *guard, int index)
{
	atomic_fetch_sub_explicit(&guard->readers[index], 1,
				  memory_order_release);
}

int stats_guard_writable(const struct stats_guard *guard, int index)
{
	return atomic_load_explicit(&guard->readers[index],
				    memory_order_acquire) == 0;
}
