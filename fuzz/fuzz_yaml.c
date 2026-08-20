/*
 * fuzz_yaml.c -- libFuzzer harness for bless's production YAML tree parser.
 *
 * The harness deliberately calls config_yaml_parse_memory(), the same
 * DPDK-independent parser used by config_init(), so parser fixes and fuzz
 * coverage cannot drift apart.
 *
 * Build (local, from the repository root):
 *   CC=clang CXX=clang++ CFLAGS="" CXXFLAGS="" \
 *     LIB_FUZZING_ENGINE="-fsanitize=fuzzer" \
 *     OUT=/tmp WORK=/tmp FUZZ_TARGETS=fuzz_yaml ZIP_TOOL=true \
 *     ./fuzz/build.sh
 */

#include <stddef.h>
#include <stdint.h>

#include "config_yaml.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	Node *root = config_yaml_parse_memory(data, size);
	config_yaml_free(root);
	return 0;
}
