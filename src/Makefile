# SPDX-License-Identifier: BSD-3-Clause
APP      := bless
BUILD    ?= release
STATIC   ?= 1
BUILD_MODE := $(if $(filter 1,$(STATIC)),static,shared)
O        ?= build/$(BUILD)-$(BUILD_MODE)
V        ?= 0
PREFIX   ?= $(shell pwd)
PKGCONF  ?= pkg-config
USE_VERSION_H  ?= 0

# Build targets depend on setup performed by the repository Makefile. Keep
# clean available here so the top-level distclean target can remove old output.
BUILD_GOALS := $(if $(MAKECMDGOALS),$(filter all install Test,$(MAKECMDGOALS)),all)
ifneq ($(BUILD_GOALS),)
ifneq ($(BLESS_TOPLEVEL_BUILD),1)
$(error src/Makefile is internal; run 'make' from the repository root)
endif
endif

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

# Records the link mode used for the current output directory.
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
# rate_psd.c and dist.c call libm directly; do not rely on libdpdk.pc to list it.
APP_SYSTEM_LIBS := -lm
# --- CRYPTO_LIBS (SSL, independent of DPDK) ---
ifeq ($(STATIC),1)
CRYPTO_LIBS  := $(shell $(PKGCONF) --static --libs libcrypto libssl 2>/dev/null || echo "-l:libssl.a -l:libcrypto.a -ldl -pthread")
CRYPTO_LIBS  := $(patsubst -lssl,-l:libssl.a,$(patsubst -lcrypto,-l:libcrypto.a,$(CRYPTO_LIBS)))
else
CRYPTO_LIBS  := $(shell $(PKGCONF) --libs libcrypto libssl 2>/dev/null || echo "-lcrypto -lssl")
endif

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

CFLAGS += -std=c11 -I../include -Wall -D_GNU_SOURCE $(DPDK_CFLAGS) $(if $(filter 1,$(USE_VERSION_H)),-DUSE_VERSION_H)

# AddressSanitizer + UndefinedBehaviorSanitizer (ASAN=1 make test)
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

# RPATH is used only by dynamically linked dependencies.
# readelf -d ./bless | grep -E 'RPATH|RUNPATH'
# LD_DEBUG=libs /path/to/bless 2>&1 | less
RPATH_FLAGS := -Wl,-rpath,\$$ORIGIN/../lib:$(PREFIX)/lib

# Translate STATIC to the buildinfo link-mode flag.
ifeq ($(STATIC),1)
BUILD_SHARED := 0
else
BUILD_SHARED := 1
endif

# ============================================================
# Build metadata. Immediate assignment keeps the values fixed for this run.
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
BUILD_LDFLAGS    := $(strip $(RPATH_FLAGS) $(LDFLAGS) $(APP_LDFLAGS) $(DPDK_LIBS) $(LDLIBS) $(CRYPTO_LIBS) $(APP_SYSTEM_LIBS))
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
.PHONY: all clean install uninstall libcivetweb libcjson Test

all: $(TARGET)

# TARGET depends on CONFIG_TRACKER.
# If STATIC changes, CONFIG_TRACKER updates, causing TARGET to re-link.
$(TARGET): $(APP_OBJ) $(CONFIG_TRACKER)
	@mkdir -p $(@D)
	$(ECHO) "  LD [$(if $(filter 1,$(STATIC)),STATIC,SHARED)] $@"
	$(Q)$(CC) $(APP_OBJ) -o $@ \
		$(RPATH_FLAGS) $(LDFLAGS) \
		$(APP_LDFLAGS) $(DPDK_LIBS) $(LDLIBS) $(CRYPTO_LIBS) \
		$(APP_SYSTEM_LIBS)

$(OBJDIR)/%.o: %.c
	@mkdir -p $(@D)
	$(ECHO) "  CC      $<"
	$(Q)$(CC) $(CFLAGS) $(APP_CFLAGS) -MMD -MP -c $< -o $@

# Embedded dashboards are generated from their source HTML.
ENTROPY_HTML_SRC := ../tools/www/entropy.html
OBSERVE_HTML_SRC := ../tools/www/observe.html
GEN_ENTROPY_PY   := ../tools/gen_entropy_inc.py
GEN_OBSERVE_PY   := ../tools/gen_observe_inc.py

entropy_html.inc: $(ENTROPY_HTML_SRC) $(GEN_ENTROPY_PY)
	$(Q)$(ECHO) "  GEN     $@"
	$(Q)python3 $(GEN_ENTROPY_PY)

observe_html.inc: $(OBSERVE_HTML_SRC) $(GEN_OBSERVE_PY)
	$(Q)$(ECHO) "  GEN     $@"
	$(Q)python3 $(GEN_OBSERVE_PY)

# server.c includes .inc files, needs explicit tracking
$(OBJDIR)/server.o: entropy_html.inc observe_html.inc chartjs.inc

# chartjs.inc: embed chart.umd.min.js as a C byte array (xxd -i -n)
CHARTJS_SRC := ../tools/www/chart.umd.min.js
chartjs.inc: $(CHARTJS_SRC)
	$(Q)$(ECHO) "  GEN     $@"
	$(Q)xxd -i -n chartjs $(CHARTJS_SRC) > $@

# server_const.c is generated from tools/www/index.html via tools/gen_hex.py
HTML_SRC    := ../tools/www/index.html
GEN_HEX_PY  := ../tools/gen_hex.py
$(OBJDIR)/server_const.o: $(GEN_HEX_PY) $(HTML_SRC)
	$(Q)$(ECHO) "  GEN     server_const.c"
	$(Q)python3 $(GEN_HEX_PY) > $(OBJDIR)/gen_hex.log 2>&1
	$(Q)grep -v '^OK:' $(OBJDIR)/gen_hex.log || true
	@mkdir -p $(@D)
	$(ECHO) "  CC      server_const.c"
	$(Q)$(CC) $(CFLAGS) $(APP_CFLAGS) -MMD -MP -c server_const.c -o $@

# Relink when STATIC changes within the same output directory.
$(CONFIG_TRACKER): FORCE
	@mkdir -p $(@D)
	@echo "STATIC=$(STATIC)" > $@.tmp
	@if [ ! -f $@ ] || [ "`cat $@`" != "`cat $@.tmp`" ]; then \
		cp $@.tmp $@; \
		echo "  CONF    Build mode changed to $(if $(filter 1,$(STATIC)),STATIC,SHARED)"; \
		fi
	@rm -f $@.tmp

# buildinfo.c holds build_time[] and build_host[]. Refresh it only after a build
# input changes so a no-op make does not trigger another link.
BUILDINFO_C := buildinfo.c
BUILDINFO_INPUTS := $(APP_SRC) $(wildcard ../include/*.h) \
	Makefile ../Makefile $(HTML_SRC) $(ENTROPY_HTML_SRC) $(OBSERVE_HTML_SRC) \
	$(GEN_HEX_PY) $(GEN_ENTROPY_PY) $(GEN_OBSERVE_PY)

$(BUILDINFO_C): $(BUILDINFO_INPUTS)
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
		echo "  GEN     $@"; \
		mv $@.tmp $@; \
	else \
		rm -f $@.tmp; \
	fi

$(OBJDIR)/buildinfo.o: $(BUILDINFO_C)
	@mkdir -p $(@D)
	$(ECHO) "  CC      $<"
	$(Q)$(CC) $(CFLAGS) $(APP_CFLAGS) -MMD -MP -c $< -o $@

# Keep the ELF note in step with buildinfo.c. It is readable with readelf -n.
$(OBJDIR)/elfnote.o: $(BUILDINFO_C)
	@mkdir -p $(@D)
	@printf '%s\n' \
	'.section .note.buildinfo,"a",@note' \
	'.align 4' \
	'.long name_end - name' \
	'.long desc_end - desc' \
	'.long 1' \
	'name:' \
	'.asciz "BLESS"' \
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
		$(DESTDIR)$(PREFIX)/www/log \
		$(DESTDIR)$(PREFIX)/include/$(APP)

	$(Q)install -m 0755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/
	$(Q)install -m 0644 ../include/*.h $(DESTDIR)$(PREFIX)/include/$(APP)/
	$(Q)install -m 0644 ../conf/config.yaml.template $(DESTDIR)$(PREFIX)/conf/

	$(Q)if ls ../lib/* >/dev/null 2>&1; then \
		echo "  INSTALL libs"; \
		install -m 0755 ../lib/*.so* $(DESTDIR)$(PREFIX)/lib/ 2>/dev/null; \
		install -m 0644 ../lib/*.a   $(DESTDIR)$(PREFIX)/lib/ 2>/dev/null; \
	fi

uninstall:
	$(Q)rm -rf $(DESTDIR)$(PREFIX)/bin/$(APP)
	$(Q)rm -rf $(DESTDIR)$(PREFIX)/include/$(APP)
	$(Q)rm -rf $(DESTDIR)$(PREFIX)/conf/config.yaml.template

Test:
	$(info >>>CFLAGS<<<)
	$(info $(CFLAGS))
	$(info >>>END<<<)
	$(info >>>BUILD_CFLAGS<<<)
	$(info $(BUILD_CFLAGS))
	$(info >>>END<<<)

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
