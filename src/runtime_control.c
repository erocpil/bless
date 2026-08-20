#include "runtime_control.h"

#include <errno.h>

int runtime_control_init(struct runtime_control *ctrl)
{
	if (!ctrl) {
		return EINVAL;
	}

	atomic_init(&ctrl->pps_rate, 0);
	atomic_init(&ctrl->entropy_target, 0.0);
	atomic_init(&ctrl->entropy_dim, 0);
	atomic_init(&ctrl->entropy_adapt_gain, 0.0);
	return pthread_mutex_init(&ctrl->mutex, NULL);
}

void runtime_control_destroy(struct runtime_control *ctrl)
{
	if (ctrl) {
		pthread_mutex_destroy(&ctrl->mutex);
	}
}

void runtime_control_lock(struct runtime_control *ctrl)
{
	pthread_mutex_lock(&ctrl->mutex);
}

void runtime_control_unlock(struct runtime_control *ctrl)
{
	pthread_mutex_unlock(&ctrl->mutex);
}

void runtime_control_publish_pps_rate(struct runtime_control *ctrl,
				      uint32_t value)
{
	atomic_store_explicit(&ctrl->pps_rate, value, memory_order_relaxed);
}

void runtime_control_publish_entropy_target(struct runtime_control *ctrl,
					    double value)
{
	atomic_store_explicit(&ctrl->entropy_target, value, memory_order_relaxed);
}

void runtime_control_publish_entropy_dim(struct runtime_control *ctrl,
					 uint8_t value)
{
	atomic_store_explicit(&ctrl->entropy_dim, value, memory_order_relaxed);
}

void runtime_control_publish_entropy_adapt_gain(struct runtime_control *ctrl,
						double value)
{
	atomic_store_explicit(&ctrl->entropy_adapt_gain, value,
			      memory_order_relaxed);
}

uint32_t runtime_control_load_pps_rate(const struct runtime_control *ctrl)
{
	return atomic_load_explicit(&ctrl->pps_rate, memory_order_relaxed);
}

double runtime_control_load_entropy_target(const struct runtime_control *ctrl)
{
	return atomic_load_explicit(&ctrl->entropy_target, memory_order_relaxed);
}

uint8_t runtime_control_load_entropy_dim(const struct runtime_control *ctrl)
{
	return atomic_load_explicit(&ctrl->entropy_dim, memory_order_relaxed);
}

double runtime_control_load_entropy_adapt_gain(
	const struct runtime_control *ctrl)
{
	return atomic_load_explicit(&ctrl->entropy_adapt_gain,
				    memory_order_relaxed);
}

bool runtime_control_compare_exchange_pps_rate(struct runtime_control *ctrl,
					       uint32_t *expected,
					       uint32_t desired)
{
	return atomic_compare_exchange_strong_explicit(
		&ctrl->pps_rate, expected, desired,
		memory_order_relaxed, memory_order_relaxed);
}
