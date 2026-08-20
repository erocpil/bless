/*
 * test_tsan_runtime.c — ThreadSanitizer test for the bless runtime
 * configuration concurrency model.
 *
 * Exercises the production runtime_control implementation used by WS set/get
 * and worker hot paths:
 *   - Writer: lock runtime_control, write plain + atomic shadow, unlock
 *   - Reader: read atomic shadow (lock-free), optionally read plain under lock
 *
 * This is DPDK-independent: the test links the real src/runtime_control.c
 * synchronization implementation.  Compile with:
 *
 *   clang -fsanitize=thread -g -pthread -I include \
 *         -o test_tsan_runtime test_tsan_runtime.c src/runtime_control.c
 *
 * Run:
 *   ./test_tsan_runtime
 *
 * Exit code 0 = TSan clean (no data races detected).
 */

#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "runtime_control.h"
#include "stats_guard.h"

/* Minimal plain-field owner.  Synchronization is the production object. */

struct mock_conf {
	/* plain fields (protected by runtime_control for writes) */
	uint32_t pps_rate;
	uint32_t sample_interval;
	double   entropy_target;

	struct runtime_control runtime;
};

/* ── test harness ── */

#define N_ITERATIONS 1000000
#define SNAPSHOT_ITERATIONS 100000

static void *writer_thread(void *arg)
{
	struct mock_conf *c = (struct mock_conf *)arg;

	for (int i = 0; i < N_ITERATIONS; i++) {
		runtime_control_lock(&c->runtime);
		/* write plain fields */
		c->pps_rate = (uint32_t)i;
		c->sample_interval = (uint32_t)(i % 1000);
		c->entropy_target = (double)(i % 100) / 10.0;
		/* publish through the production accessors */
		runtime_control_publish_pps_rate(&c->runtime, c->pps_rate);
		runtime_control_publish_entropy_target(
			&c->runtime, c->entropy_target);
		runtime_control_publish_entropy_dim(
			&c->runtime, (uint8_t)(i % 9));
		runtime_control_publish_entropy_adapt_gain(
			&c->runtime, (double)(i % 50));
		runtime_control_unlock(&c->runtime);
	}

	return NULL;
}

static void *reader_thread(void *arg)
{
	struct mock_conf *c = (struct mock_conf *)arg;

	for (int i = 0; i < N_ITERATIONS; i++) {
		/* read atomic shadows (lock-free, happens in the
		 * production hot path via worker_func_tx_only) */
		uint32_t pps = runtime_control_load_pps_rate(&c->runtime);
		double target =
			runtime_control_load_entropy_target(&c->runtime);
		uint8_t dim = runtime_control_load_entropy_dim(&c->runtime);
		double gain =
			runtime_control_load_entropy_adapt_gain(&c->runtime);

		/* read plain field under lock (production WS "get") */
		runtime_control_lock(&c->runtime);
		double ent = c->entropy_target;
		uint32_t si = c->sample_interval;
		runtime_control_unlock(&c->runtime);

		/* prevent compiler from optimising away reads */
		volatile uint32_t v_pps = pps;
		volatile uint32_t v_si  = si;
		volatile double   v_ent = ent;
		volatile double   v_target = target;
		volatile uint8_t  v_dim = dim;
		volatile double   v_gain = gain;
		(void)v_pps; (void)v_si; (void)v_ent;
		(void)v_target; (void)v_dim; (void)v_gain;
	}

	return NULL;
}

struct snapshot_payload {
	uint64_t begin;
	uint64_t value;
	uint64_t end;
};

struct snapshot_test {
	struct stats_guard guard;
	struct snapshot_payload slots[2];
};

static void *snapshot_writer(void *arg)
{
	struct snapshot_test *test = arg;

	for (uint64_t sequence = 1; sequence <= SNAPSHOT_ITERATIONS;
	     sequence++) {
		int inactive = stats_guard_active(&test->guard) ^ 1;
		while (!stats_guard_writable(&test->guard, inactive))
			sched_yield();

		test->slots[inactive].begin = sequence;
		test->slots[inactive].value = sequence * 3;
		test->slots[inactive].end = sequence;
		stats_guard_publish(&test->guard, inactive);
	}
	return NULL;
}

static void *snapshot_reader(void *arg)
{
	struct snapshot_test *test = arg;

	for (int i = 0; i < SNAPSHOT_ITERATIONS; i++) {
		int index = stats_guard_acquire(&test->guard);
		struct snapshot_payload copy = test->slots[index];
		stats_guard_release(&test->guard, index);

		if (copy.begin != copy.end || (copy.begin != 0 && copy.value != copy.begin * 3)) {
			abort();
		}
	}
	return NULL;
}

int main(void)
{
	struct mock_conf conf;
	memset(&conf, 0, sizeof(conf));
	if (runtime_control_init(&conf.runtime) != 0) {
		fprintf(stderr, "runtime_control_init failed\n");
		return 1;
	}

	pthread_t writer_tid, reader_tid;
	pthread_t writers[4], readers[4];

	printf("TSan runtime-config concurrency test (%d iterations)\n",
	       N_ITERATIONS);

	/* single-writer single-reader (most common production pattern) */
	pthread_create(&writer_tid, NULL, writer_thread, &conf);
	pthread_create(&reader_tid, NULL, reader_thread, &conf);
	pthread_join(writer_tid, NULL);
	pthread_join(reader_tid, NULL);

	/* multi-writer multi-reader (stress test) */
	printf("  SW/SR passed — starting MW/MR stress\n");
	for (int i = 0; i < 4; i++) {
		pthread_create(&writers[i], NULL, writer_thread, &conf);
		pthread_create(&readers[i], NULL, reader_thread, &conf);
	}
	for (int i = 0; i < 4; i++) {
		pthread_join(writers[i], NULL);
		pthread_join(readers[i], NULL);
	}

	runtime_control_destroy(&conf.runtime);

	struct snapshot_test snapshot = {0};
	atomic_init(&snapshot.guard.active, 0);
	atomic_init(&snapshot.guard.readers[0], 0);
	atomic_init(&snapshot.guard.readers[1], 0);

	printf("  runtime control passed — starting snapshot pin/retry stress\n");
	pthread_create(&writer_tid, NULL, snapshot_writer, &snapshot);
	for (int i = 0; i < 4; i++)
		pthread_create(&readers[i], NULL, snapshot_reader, &snapshot);
	pthread_join(writer_tid, NULL);
	for (int i = 0; i < 4; i++)
		pthread_join(readers[i], NULL);

	printf("TSan PASS — production synchronization modules are race-free\n");
	return 0;
}
