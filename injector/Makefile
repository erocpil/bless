# ============================================================
# Top-level Makefile
# ============================================================

# ----------------------------
# global parameters, over written by command line
# ----------------------------
BUILD    ?= release
STATIC   ?= 1
V        ?= 0
PREFIX   ?= /opt/injector-1.0
DESTDIR  ?=
USE_VERSION_H  ?= 1
INJECTOR_VERSION ?= 1.0

# ----------------------------
# build mode
# ----------------------------
BUILD_MODE := $(if $(filter 1,$(STATIC)),static,shared)
O          ?= build/$(BUILD)-$(BUILD_MODE)

# ----------------------------
# path definition
# ----------------------------
TOPDIR   := $(CURDIR)
ROOT     := $(abspath .)
ABS_O    := $(abspath $(O))
SRCDIR   := src

# ----------------------------
# sub-make for src/
# ----------------------------
MAKE_SRC := $(MAKE) -C $(SRCDIR) \
			O="$(ABS_O)" \
			RUNTIME_LIB="$(abspath ../build/runtime/libbless_dpdk_runtime.a)" \
			BUILD="$(BUILD)" \
			STATIC="$(STATIC)" \
			V="$(V)" \
			PREFIX="$(PREFIX)" \
			DESTDIR="$(DESTDIR)" \
			USE_VERSION_H="$(USE_VERSION_H)"

.DEFAULT_GOAL := all

# version header file
VERSION_H = include/version.h

# ============================================================
# clean targets
# ============================================================
# use ifneq check clean/distclean/uninstall/help
ifneq ($(filter clean distclean uninstall help,$(MAKECMDGOALS)),)

.PHONY: clean distclean help
clean:
	@echo "  CLEANING build directory..."
	rm -rf build/

distclean: clean
	$(MAKE) -C src clean
	rm -f include/version.h src/buildinfo.c

uninstall:
	+$(MAKE_SRC) uninstall

help:
	@echo "Usage:"
	@echo "  make [target] [VARIABLE=value]"
	@echo ""
	@echo "Targets:"
	@echo "  all        Build everything (default)"
	@echo "  install    Install binaries"
	@echo "  clean      Remove build artifacts"
	@echo "  distclean  Clean everything"
	@echo ""
	@echo "Variables:"
	@echo "  STATIC=1|0     Build static or shared (default: 1)"
	@echo "  BUILD=release|debug (default: release)"
	@echo "  PREFIX=/path  Install prefix"

else

# ============================================================
# standard procedure
# ============================================================

GIT_COMMIT := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)
GIT_STATE := $(shell \
        if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then \
                if git diff-index --quiet HEAD --; then \
                        echo clean; \
                else \
                        echo dirty; \
                fi; \
        else \
                echo nogit; \
        fi)

# version.h: stable fields only (GIT_COMMIT, BRANCH, STATE do not change
# between invocations).  Uses content-guard — only rewrites the file and
# updates its mtime when the content actually changed.  This prevents a
# cascade rebuild of every translation unit that includes base.h → version.h.
# BUILD_TIME and BUILD_HOST live in src/buildinfo.c (FORCE, one .o only).
FORCE:
$(VERSION_H): FORCE
	@echo "  GEN       $@"
	@{ \
		echo "#ifndef VERSION_H"; \
		echo "#define VERSION_H"; \
		echo ""; \
		echo "#define INJECTOR_VERSION \"$(INJECTOR_VERSION)\""; \
		echo "#define GIT_COMMIT \"$(GIT_COMMIT)\""; \
		echo "#define GIT_STATE \"$(GIT_STATE)\""; \
		echo "#define GIT_BRANCH \"$$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)\""; \
		echo "#define BUILD_TYPE \"$(BUILD)\""; \
		echo "#define STATIC $(STATIC)"; \
		echo ""; \
		echo "#endif /* VERSION_H */"; \
	} > $@.tmp
	@if [ ! -f $@ ] || ! cmp -s $@.tmp $@; then \
		mv $@.tmp $@; \
	else \
		rm -f $@.tmp; \
	fi

# ============================================================
# targets
# ============================================================

.PHONY: all install uninstall

# default
all: $(VERSION_H)
	+$(MAKE_SRC)

install: $(VERSION_H)
	+$(MAKE_SRC) install

endif
