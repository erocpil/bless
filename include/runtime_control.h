#ifndef BLESS_RUNTIME_CONTROL_H
#define BLESS_RUNTIME_CONTROL_H

#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>

/*
 * DPDK-independent synchronization object used by the production runtime
 * configuration path.  Plain bless_conf fields are protected by mutex;
 * workers consume the atomic shadows through the accessors below.
 */
struct runtime_control {
	pthread_mutex_t mutex;
	_Atomic uint32_t pps_rate;
	_Atomic double entropy_target;
	_Atomic uint8_t entropy_dim;
	_Atomic double entropy_adapt_gain;
};

int runtime_control_init(struct runtime_control *ctrl);
void runtime_control_destroy(struct runtime_control *ctrl);
void runtime_control_lock(struct runtime_control *ctrl);
void runtime_control_unlock(struct runtime_control *ctrl);

void runtime_control_publish_pps_rate(struct runtime_control *ctrl,
				      uint32_t value);
void runtime_control_publish_entropy_target(struct runtime_control *ctrl,
					    double value);
void runtime_control_publish_entropy_dim(struct runtime_control *ctrl,
					 uint8_t value);
void runtime_control_publish_entropy_adapt_gain(struct runtime_control *ctrl,
						double value);

uint32_t runtime_control_load_pps_rate(const struct runtime_control *ctrl);
double runtime_control_load_entropy_target(const struct runtime_control *ctrl);
uint8_t runtime_control_load_entropy_dim(const struct runtime_control *ctrl);
double runtime_control_load_entropy_adapt_gain(
	const struct runtime_control *ctrl);
bool runtime_control_compare_exchange_pps_rate(struct runtime_control *ctrl,
					       uint32_t *expected,
					       uint32_t desired);

#endif
