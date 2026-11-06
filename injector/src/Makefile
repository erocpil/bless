# SPDX-License-Identifier: BSD-3-Clause
APP      := injector
BUILD    ?= release
O        ?= build/$(BUILD)-shared
STATIC   ?= 1
V        ?= 0
PREFIX   ?= $(shell pwd)
PKGCONF  ?= pkg-config
USE_VERSION_H  ?= 0
RUNTIME_LIB ?= ../../build/runtime/libbless_dpdk_runtime.a

# ============================================================
# Compiler selection and flags
# ============================================================
ifeq ($(origin CC), default)
CLANG := $(shell command -v clang 2>/dev/null)
ifeq ($(shell echo 'int main(){}' | clang -x c - -o /dev/null 2>/dev/null && echo yes),yes)
CC := clang
else
CC := gcc
endif
endif

MAKEFLAGS += -rR
.SUFFIXES:

OBJDIR := $(O)/obj
BINDIR := $(O)/bin
TARGET := $(BINDIR)/$(APP)

# Config tracker: records the current build mode
CONFIG_TRACKER  := $(OBJDIR)/.build_config

APP_SRC  := $(filter-out buildinfo.c,$(wildcard *.c))
APP_OBJ  := $(APP_SRC:%.c=$(OBJDIR)/%.o)
APP_OBJ  += $(OBJDIR)/buildinfo.o
APP_OBJ  += $(OBJDIR)/elfnote.o
APP_DEPS := $(APP_SRC:%.c=$(OBJDIR)/%.d)
APP_DEPS += $(OBJDIR)/buildinfo.d

# ============================================================
# Compilation and linking
# ============================================================
DPDK_CFLAGS  := $(shell $(PKGCONF) --cflags libdpdk 2>/dev/null) -DALLOW_EXPERIMENTAL_API
DPDK_LDFLAGS := $(shell $(PKGCONF) $(if $(filter 1,$(STATIC)),--static) --libs libdpdk 2>/dev/null)
# --- DPDK_LIBS (fully static, except glibc core libs and libsystemd) ---
#   libsystemd on Debian 12 has no .a: must stay dynamic
#   libm.a (glibc 2.36) IFUNC resolver references _dl_x86_cpu_features
#   (only provided by dynamic linker). Same for libpthread.a, libdl.a:
#   keep dynamic to avoid IFUNC undefined references.
#   Set EXTRA_LIBDIR for vendor-specific paths (e.g. Mellanox libmtcr_ul.a).
#   Other system deps (libnl, libcap, libgcrypt...) all have .a.
STAY_DYNAMIC := -lsystemd -lm -lpthread -ldl
# libisal has no .a on Ubuntu 24.04 (.so only), must stay dynamic.
# Only add to dynamic segment when DPDK declares -lisal and no .a exists.
DPDK_NEED_ISAL := $(filter -lisal,$(DPDK_LDFLAGS))
ifneq ($(DPDK_NEED_ISAL),)
  ifeq ($(wildcard /usr/lib/x86_64-linux-gnu/libisal.a /usr/lib/libisal.a),)
    STAY_DYNAMIC += -lisal
  endif
endif
DPDK_FOR_BSTATIC := $(filter-out $(STAY_DYNAMIC),$(DPDK_LDFLAGS))
EXTRA_LIBDIR ?=
ifeq ($(STATIC),1)
DPDK_LIBS := $(if $(EXTRA_LIBDIR),-L$(EXTRA_LIBDIR)) \
             -Wl,-Bstatic \
             $(DPDK_FOR_BSTATIC) \
             -Wl,-Bdynamic \
             $(STAY_DYNAMIC)
else
DPDK_LIBS := $(DPDK_LDFLAGS)
endif

# Reuse the repository-level pinned libyaml archive in both DPDK link modes.
YAML_LIB := $(abspath ../../lib/libyaml.a)
LDLIBS += $(YAML_LIB) -lm $(if $(filter 1,$(STATIC)),-l:libcrypto.a,-lcrypto) -ldl -pthread

CFLAGS += -std=c11 -I../include -I../../runtime/dpdk/include -Wall -D_GNU_SOURCE $(DPDK_CFLAGS) $(if $(filter 1,$(USE_VERSION_H)),-DUSE_VERSION_H)

# AddressSanitizer + UndefinedBehaviorSanitizer (make ASAN=1)
ifeq ($(ASAN),1)
CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer -g
LDFLAGS += -fsanitize=address,undefined
endif

ifeq ($(BUILD),debug)
CFLAGS += -O0 -g -DDEBUG
else
CFLAGS += -O2 -DNDEBUG
endif

ifeq ($(CC),clang)
XAPP_CFLAGS += -Weverything -Wno-padded -Wno-declaration-after-statement
else
CFLAGS += -Wextra -Wpedantic -Wstrict-prototypes
endif

# RPATH only meaningful for dynamic linking, but kept in variable.
# readelf -d ./injector | grep -E 'RPATH|RUNPATH'
# LD_DEBUG=libs /path/to/injector 2>&1 | less
RPATH_FLAGS := -Wl,-rpath,\$$ORIGIN/../lib:$(PREFIX)/lib

# Select link mode based on STATIC
ifeq ($(STATIC),1)
BUILD_SHARED := 0
else
BUILD_SHARED := 1
endif

# ============================================================
# Gather build info.
# := prevents recursive expansion.
# ============================================================
BUILD_GIT_BRANCH := $(shell git rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)
BUILD_GIT_COMMIT := $(shell \
if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then \
	desc=$$(git rev-parse --short HEAD); \
	if [ -n "$$(git status --porcelain)" ]; then \
		echo "$$desc-dirty"; \
	else \
		echo "$$desc"; \
	fi; \
else \
	echo unknown; \
fi)
# Reproducible builds: SOURCE_DATE_EPOCH pins timestamps and hostname.
ifdef SOURCE_DATE_EPOCH
BUILD_HOST       := reproducible
BUILD_TIME       := $(shell date -u -d @$(SOURCE_DATE_EPOCH) '+%Y-%m-%d %H:%M:%S UTC')
else
BUILD_HOST       := $(shell hostname)
BUILD_TIME       := $(shell date -u '+%Y-%m-%d %H:%M:%S UTC')
endif
BUILD_CC_VERSION := $(shell $(CC) --version | head -n 1)
BUILD_CFLAGS     := $(strip $(CFLAGS) $(APP_CFLAGS))
BUILD_LDFLAGS    := $(strip $(RPATH_FLAGS) $(LDFLAGS) $(APP_LDFLAGS) $(DPDK_LIBS) $(LDLIBS))
BUILDINFO_DESC   := git_branch=$(BUILD_GIT_BRANCH)\n\
	git_commit=$(BUILD_GIT_COMMIT)\n\
	build_host=$(BUILD_HOST)\n\
	build_time=$(BUILD_TIME)\n\
	cc_version=$(BUILD_CC_VERSION)\n\
	CFLAGS='$(BUILD_CFLAGS)'\n\
	LDFLAGS='$(BUILD_LDFLAGS)'\n
BUILDINFO_SIZE   := $(shell printf '%b' "$(BUILDINFO_DESC)" | wc -c)
BUILDINFO_PAD    := $(shell \
	s=$(BUILDINFO_SIZE); \
	echo $$(( (4 - ($$s % 4)) % 4 )) \
)

# ============================================================
# Build rules
# ============================================================
.PHONY: all clean install uninstall

all: $(TARGET)

# TARGET depends on CONFIG_TRACKER.
# If STATIC changes, CONFIG_TRACKER updates, causing TARGET to re-link.
$(TARGET): $(APP_OBJ) $(CONFIG_TRACKER) $(YAML_LIB) $(RUNTIME_LIB)
	@mkdir -p $(@D)
	$(ECHO) "  LD [$(if $(filter 1,$(STATIC)),STATIC,SHARED)] $@"
	$(Q)$(CC) $(APP_OBJ) -o $@ \
		$(RPATH_FLAGS) $(LDFLAGS) \
		$(APP_LDFLAGS) $(RUNTIME_LIB) $(DPDK_LIBS) $(LDLIBS)

$(OBJDIR)/%.o: %.c
	@mkdir -p $(@D)
	$(ECHO) "  CC      $<"
	$(Q)$(CC) $(CFLAGS) $(APP_CFLAGS) -MMD -MP -c $< -o $@

# Config tracker: detect when STATIC changes
$(CONFIG_TRACKER): FORCE
	@mkdir -p $(@D)
	@echo "STATIC=$(STATIC)" > $@.tmp
	@if [ ! -f $@ ] || [ "`cat $@`" != "`cat $@.tmp`" ]; then \
		cp $@.tmp $@; \
		echo "  CONF    Build mode changed to $(if $(filter 1,$(STATIC)),STATIC,SHARED)"; \
		fi
	@rm -f $@.tmp

# buildinfo.c: generated C source -- holds build_time[] and build_host[].
# These are the only fields that change on every make.  Isolating them
# in a single .c means only one .o is rebuilt per invocation, instead of
# cascading through version.h to every translation unit.
BUILDINFO_C := buildinfo.c

$(BUILDINFO_C): FORCE
	@echo "  GEN       $@"
	@{ \
		echo '/* Generated by Makefile -- DO NOT EDIT */'; \
		echo '#include "buildinfo.h"'; \
		echo ''; \
		if [ -n "$$SOURCE_DATE_EPOCH" ]; then \
			echo "const char build_time[] = \"$$(date -u -d @$$SOURCE_DATE_EPOCH '+%Y-%m-%d %H:%M:%S UTC')\";"; \
			echo 'const char build_host[] = "reproducible";'; \
		else \
			echo "const char build_time[] = \"$$(date '+%Y-%m-%d %H:%M:%S %Z')\";"; \
			echo "const char build_host[] = \"$$(hostname)\";"; \
		fi; \
	} > $@.tmp
	@if [ ! -f $@ ] || ! cmp -s $@.tmp $@; then \
		mv $@.tmp $@; \
	else \
		rm -f $@.tmp; \
	fi

$(OBJDIR)/buildinfo.o: $(BUILDINFO_C)
	@mkdir -p $(@D)
	$(ECHO) "  CC      $<"
	$(Q)$(CC) $(CFLAGS) $(APP_CFLAGS) -MMD -MP -c $< -o $@

# ELF note (.note.buildinfo): embedded in the binary, readable via
# readelf -n.  Content-guarded: only recompiles when BUILDINFO_DESC
# actually changes (e.g. build_time tick, cc_version change).
$(OBJDIR)/elfnote.o: FORCE
	@mkdir -p $(@D)
	@printf '%s\n' \
	'.section .note.buildinfo,"a",@note' \
	'.align 4' \
	'.long name_end - name' \
	'.long desc_end - desc' \
	'.long 1' \
	'name:' \
	'.asciz "INJECTOR"' \
	'name_end:' \
	'.align 4' \
	'desc:' \
	'.ascii "$(BUILDINFO_DESC)"' \
	'desc_end:' \
	'.align 4' \
	'.section .note.GNU-stack,"",@progbits' \
	> $@.tmp
	@if ! cmp -s $@.tmp $@.stamp 2>/dev/null; then \
		echo "  AS      elfnote.o"; \
		cat $@.tmp | $(CC) -c -x assembler -o $@ - && cp $@.tmp $@.stamp; \
	fi
	@rm -f $@.tmp

.PHONY: FORCE
FORCE:

# ============================================================
# Install and uninstall
# ============================================================
install: all
	$(ECHO) "  INSTALL to $(DESTDIR)$(PREFIX)"
	$(Q)install -d \
		$(DESTDIR)$(PREFIX)/bin \
		$(DESTDIR)$(PREFIX)/lib \
		$(DESTDIR)$(PREFIX)/conf \
		$(DESTDIR)$(PREFIX)/include/$(APP)

	$(Q)install -m 0755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/
	$(Q)install -m 0644 ../include/*.h $(DESTDIR)$(PREFIX)/include/$(APP)/
	$(Q)install -m 0644 ../conf/config.yaml $(DESTDIR)$(PREFIX)/conf/

	$(Q)if ls ../lib/* >/dev/null 2>&1; then \
		echo "  INSTALL libs"; \
		install -m 0755 ../lib/*.so* $(DESTDIR)$(PREFIX)/lib/ 2>/dev/null; \
		install -m 0644 ../lib/*.a   $(DESTDIR)$(PREFIX)/lib/ 2>/dev/null; \
	fi

uninstall:
	$(Q)rm -rf $(DESTDIR)$(PREFIX)/bin/$(APP)
	$(Q)rm -rf $(DESTDIR)$(PREFIX)/include/$(APP)
	$(Q)rm -rf $(DESTDIR)$(PREFIX)/conf/config.yaml

# Include auto-generated dependency files
-include $(APP_DEPS)

# Verbosity control
ifeq ($(V),1)
Q :=
ECHO := @echo
else
Q := @
ECHO := @echo
endif

clean:
	$(Q)rm -rf $(O)
	$(Q)rm -rf build/
