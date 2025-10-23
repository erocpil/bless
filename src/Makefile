# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2010-2014 Intel Corporation

# binary name
APP = bless
BUILDDIR ?= build

# all source are stored in SRCS-y
SRCS-y := $(wildcard *.c)
OBJS-y := $(patsubst %.c, $(BUILDDIR)/%.o, $(SRCS-y))

PKGCONF ?= pkg-config

# Build using pkg-config variables if possible
ifneq ($(shell $(PKGCONF) --exists libdpdk && echo 0),0)
$(error "no installation of DPDK found")
endif

all: shared
	echo $(OBJS-y)

.PHONY: shared static
shared: $(BUILDDIR)/$(APP)-shared
	ln -sf $(APP)-shared $(BUILDDIR)/$(APP)
static: $(BUILDDIR)/$(APP)-static
	ln -sf $(APP)-static $(BUILDDIR)/$(APP)

PC_FILE := $(shell $(PKGCONF) --path libdpdk 2>/dev/null)
CFLAGS += -O2 $(shell $(PKGCONF) --cflags libdpdk)
# CFLAGS += -Wall -g -O0 $(shell $(PKGCONF) --cflags libdpdk)
CFLAGS += -I../include
# Add flag to allow experimental API as bless uses rte_ethdev_set_ptype API
CFLAGS += -DALLOW_EXPERIMENTAL_API
LDFLAGS_SHARED = $(shell $(PKGCONF) --libs libdpdk) -lyaml -lcrypto
LDFLAGS_STATIC = $(shell $(PKGCONF) --static --libs libdpdk) -lyaml -lcrypto

ifeq ($(MAKECMDGOALS),static)
# check for broken pkg-config
ifeq ($(shell echo $(LDFLAGS_STATIC) | grep 'whole-archive.*l:lib.*no-whole-archive'),)
$(warning "pkg-config output list does not contain drivers between 'whole-archive'/'no-whole-archive' flags.")
$(error "Cannot generate statically-linked binaries with this version of pkg-config")
endif
endif

$(BUILDDIR)/$(APP)-shared: $(SRCS-y) Makefile $(PC_FILE) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(SRCS-y) -o $@ $(LDFLAGS) $(LDFLAGS_SHARED)

$(BUILDDIR)/$(APP)-static: $(SRCS-y) Makefile $(PC_FILE) | $(BUILDDIR)
	$(CC) $(CFLAGS) $(SRCS-y) -o $@ $(LDFLAGS) $(LDFLAGS_STATIC)

$(BUILDDIR):
	@mkdir -p $@

.PHONY: clean
clean:
	rm -f $(BUILDDIR)/$(APP) $(BUILDDIR)/$(APP)-static $(BUILDDIR)/$(APP)-shared
	# test -d $(BUILDDIR) && rmdir -p $(BUILDDIR) || true
	test -d $(BUILDDIR) && rmdir $(BUILDDIR) || true
