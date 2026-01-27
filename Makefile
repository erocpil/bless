# ============================================================
# Top-level Makefile
# ============================================================

# ----------------------------
# global parameters, over written by command line
# ----------------------------
BUILD    ?= release
STATIC   ?= 1
V        ?= 0
PREFIX   ?= /opt/bless-1.0
DESTDIR  ?=
USE_VERSION_H  ?= 1
BL_VERSION ?= 1.0

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

# upstream .mk
UP_MK    := $(ROOT)/third_party/third_party.mk

# ----------------------------
# sub-make for src/
# ----------------------------
MAKE_SRC := $(MAKE) -C $(SRCDIR) \
			O="$(ABS_O)" \
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
	$(MAKE) -C third_party clean
	$(MAKE) -C src clean

uninstall:
	$(MAKE_SRC) uninstall

help:
	@echo "Usage:"
	@echo "  make [target] [VARIABLE=value]"
	@echo ""
	@echo "Targets:"
	@echo "  all        Build everything (default)"
	@echo "  upstream   Prepare third-party libraries only"
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

# ============================================================
# third_party.mk rules
# ============================================================

# no update-third-party dependency
$(UP_MK):
	@echo "  GENERATING $@..."
	$(MAKE) -C third_party third_party.mk

# manually update
.PHONY: update-third-party

update-third-party:
	$(MAKE) -C third_party update-third-party

# only third_party (for CI / debug)
third_party: $(UP_MK)

# ============================================================
# include third_party output
# ============================================================
include $(UP_MK)

GIT_COMMIT := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)
GIT_STATE  := $(shell \
	if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then \
		if [ -n "$$(git status --porcelain 2>/dev/null)" ]; then \
			echo dirty; \
		else \
			echo clean; \
		fi; \
	else \
		echo nogit; \
	fi)

# generate new version.h for every make
$(VERSION_H):
	@echo "Generating $@..."
	@echo "#ifndef VERSION_H" > $@
	@echo "#define VERSION_H" >> $@
	@echo "" >> $@
	@echo '#define BL_VERSION "$(BL_VERSION)"' >> $@
	@echo "#define GIT_COMMIT \"$(GIT_COMMIT)\"" >> $@
	@echo "#define GIT_STATE \"$(GIT_STATE)\"" >> $@
	@echo "#define GIT_BRANCH \"$$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)\"" >> $@
	@echo "#define BUILD_TIME \"$$(date '+%Y-%m-%d %H:%M:%S')\"" >> $@
	@echo "#define BUILD_HOST \"$$(hostname)\"" >> $@
	@echo "#define BUILD_TYPE \"$(BUILD)\"" >> $@
	@echo "#define STATIC $(STATIC)" >> $@
	@echo "" >> $@
	@echo "#endif /* VERSION_H */" >> $@

# ============================================================
# export third party varibles to src
# ============================================================

export CFLAGS  := $(CFLAGS) $(THIRD_PARTY_CFLAGS)
export LDFLAGS := $(LDFLAGS) $(THIRD_PARTY_LDFLAGS)
export LDLIBS  := $(LDLIBS) $(THIRD_PARTY_LDLIBS)

# ============================================================
# targets
# ============================================================

.PHONY: all upstream install uninstall $(VERSION_H) Test

# default
all: $(UP_MK) $(VERSION_H)
	$(MAKE_SRC)

upstream: $(UP_MK)

Test:
	$(MAKE_SRC) Test

install: $(UP_MK)
	$(MAKE_SRC) install

endif

check:
	readelf -d build/release-static/bin/bless | grep PATH
	llvm-readobj --notes build/release-static/bin/bless
	@echo "more checks:"
	@echo "  objdump -s -j .note.buildinfo build/release-static/bin/bless"
	@echo "  readelf -x .note.buildinfo build/release-static/bin/bless"
	@echo "  readelf -n build/release-static/bin/bless | \
		sed -n '/Displaying notes found in: .note.buildinfo/,/Displaying notes found in:/p' | \
		sed -n '/description data:/,/)/p' | \
		sed -n '/description data:/,/)/p'"
