# SPDX-License-Identifier: BSD-3-Clause
CC ?= cc
PKGCONF ?= pkg-config
O ?= build
DPDK_CFLAGS := $(shell $(PKGCONF) --cflags libdpdk 2>/dev/null)
OBJ := $(O)/bless_dpdk_runtime.o
LIB := $(O)/libbless_dpdk_runtime.a
TEST := $(O)/test_runtime
DPDK_TEST_LIBS := $(shell $(PKGCONF) --libs libdpdk 2>/dev/null)

.PHONY: all test clean
all: $(LIB)

test: $(TEST)
	$(TEST)

$(O):
	mkdir -p $@

$(OBJ): src/bless_dpdk_runtime.c include/bless_dpdk_runtime.h | $(O)
	$(CC) -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror $(DPDK_CFLAGS) -Iinclude -c $< -o $@

$(LIB): $(OBJ)
	$(AR) rcs $@ $^

$(TEST): tests/test_runtime.c $(LIB)
	$(CC) -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror $(DPDK_CFLAGS) \
		-Iinclude $< $(LIB) $(DPDK_TEST_LIBS) -o $@

clean:
	rm -rf build
