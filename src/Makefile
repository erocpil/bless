# SPDX-License-Identifier: BSD-3-Clause
APP      := bless
BUILD    ?= release
O        ?= build/$(BUILD)-shared
STATIC   ?= 1
V        ?= 0
PREFIX   ?= $(shell pwd)
PKGCONF  ?= pkg-config
USE_VERSION_H  ?= 0

# ============================================================
# 编译器选择与标志
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

# 配置追踪器文件：记录当前的编译模式
CONFIG_TRACKER  := $(OBJDIR)/.build_config
BUILDINFO_STAMP := $(OBJDIR)/.buildinfo.stamp

APP_SRC  := $(wildcard *.c)
APP_OBJ  := $(APP_SRC:%.c=$(OBJDIR)/%.o)
APP_OBJ  += $(OBJDIR)/buildinfo.o
APP_DEPS := $(APP_SRC:%.c=$(OBJDIR)/%.d)
# ============================================================
# 编译链接逻辑
# ============================================================
DPDK_CFLAGS  := $(shell $(PKGCONF) --cflags libdpdk 2>/dev/null) -DALLOW_EXPERIMENTAL_API
DPDK_LDFLAGS := $(shell $(PKGCONF) $(if $(filter 1,$(STATIC)),--static) --libs libdpdk 2>/dev/null)
CRYPTO_LIBS  := $(shell $(PKGCONF) --libs libcrypto 2>/dev/null || echo "-lcrypto")

CFLAGS += -std=c11 -I../include -Wall $(DPDK_CFLAGS) $(if $(filter 1,$(USE_VERSION_H)),-DUSE_VERSION_H)

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

# RPATH 仅对动态链接有意义，但保留在变量中
# readelf -d ./bless | grep -E 'RPATH|RUNPATH'
# LD_DEBUG=libs /path/to/bless 2>&1 | less
RPATH_FLAGS := -Wl,-rpath,\$$ORIGIN/../lib:$(PREFIX)/lib

# 根据 STATIC 决定链接方式
ifeq ($(STATIC),1)
BUILD_SHARED := 0
else
BUILD_SHARED := 1
endif

# ============================================================
# 收集信息
# 这里用 :=，避免递归展开
# ============================================================
BUILD_GIT_BRANCH := $(shell git rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)
BUILD_GIT_COMMIT := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)
BUILD_HOST       := $(shell hostname)
BUILD_TIME       := $(shell date -u '+%Y-%m-%d %H:%M:%S UTC')
BUILD_CC_VERSION := $(shell $(CC) --version | head -n 1)
BUILD_CFLAGS     := $(CFLAGS) $(APP_CFLAGS)
BUILD_LDFLAGS    := $(RPATH_FLAGS) $(LDFLAGS) $(APP_LDFLAGS) $(DPDK_LDFLAGS) $(CRYPTO_LIBS) $(LDLIBS)

BUILDINFO_DESC := git_branch=$(BUILD_GIT_BRANCH)\n\
	git_commit=$(BUILD_GIT_COMMIT)\n\
	build_host=$(BUILD_HOST)\n\
	build_time=$(BUILD_TIME)\n\
	cc_version=$(BUILD_CC_VERSION)\n\
	CFLAGS='$(BUILD_CFLAGS)'\n\
	LDFLAGS='$(BUILD_LDFLAGS)'\n
BUILDINFO_SIZE := $(shell printf '%b' "$(BUILDINFO_DESC)" | wc -c)
BUILDINFO_PAD := $(shell \
	s=$(BUILDINFO_SIZE); \
	echo $$(( (4 - ($$s % 4)) % 4 )) \
)

# ============================================================
# 构建规则
# ============================================================
.PHONY: all clean install uninstall libcivetweb libcjson

all: $(TARGET)

# TARGET 依赖 CONFIG_TRACKER
# 如果 STATIC 改变，CONFIG_TRACKER 会更新，导致 TARGET 重新链接
$(TARGET): $(APP_OBJ) $(CONFIG_TRACKER)
	@mkdir -p $(@D)
	$(ECHO) "  LD [$(if $(filter 1,$(STATIC)),STATIC,SHARED)] $@"
	$(Q)$(CC) $(APP_OBJ) -o $@ \
		$(RPATH_FLAGS) $(LDFLAGS) \
		$(APP_LDFLAGS) $(DPDK_LDFLAGS) $(CRYPTO_LIBS) $(LDLIBS)

$(OBJDIR)/%.o: %.c
	@mkdir -p $(@D)
	$(ECHO) "  CC      $<"
	$(Q)$(CC) $(CFLAGS) $(APP_CFLAGS) -MMD -MP -c $< -o $@

# 配置追踪器：检测 STATIC 变量是否发生变化
$(CONFIG_TRACKER): FORCE
	@mkdir -p $(@D)
	@echo "STATIC=$(STATIC)" > $@.tmp
	@if [ ! -f $@ ] || [ "`cat $@`" != "`cat $@.tmp`" ]; then \
		cp $@.tmp $@; \
		echo "  CONF    Build mode changed to $(if $(filter 1,$(STATIC)),STATIC,SHARED)"; \
		fi
	@rm -f $@.tmp

$(BUILDINFO_STAMP): FORCE
	@mkdir -p $(@D)
	@echo "BUILD_TIME: $(BUILD_TIME)" > $@
	@echo "BUILD_HOST: $(BUILD_HOST)" >> $@
	@echo "BUILD_GIT_COMMIT: $(BUILD_GIT_COMMIT)" >> $@
	@echo "BUILD_GIT_BRANCH: $(BUILD_GIT_BRANCH)" >> $@
	@echo "BUILD_CC_VERSION: $(BUILD_CC_VERSION)" >> $@

$(OBJDIR)/buildinfo.o: FORCE
	$(Q)mkdir -p $(@D)
	$(Q)printf '%s\n' \
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
	| $(CC) -c -x assembler -o $@ -

.PNONY: FORCE
FORCE:

# ============================================================
# 安装与卸载逻辑
# ============================================================
install: all
	$(ECHO) "  INSTALL to $(DESTDIR)$(PREFIX)"
	$(Q)install -d $(DESTDIR)$(PREFIX)/bin \
		$(DESTDIR)$(PREFIX)/lib \
		$(DESTDIR)$(PREFIX)/conf \
		$(DESTDIR)$(PREFIX)/www \
		$(DESTDIR)$(PREFIX)/www/log \
		$(DESTDIR)$(PREFIX)/include/$(APP)
	$(Q)install -m 0755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/
	$(Q)install -m 0644 ../www/index.html $(DESTDIR)$(PREFIX)/www/

uninstall:
	$(Q)rm -f $(DESTDIR)$(PREFIX)/bin/$(APP)
	$(Q)rm -rf $(DESTDIR)$(PREFIX)/include/$(APP)

# 包含自动生成的依赖文件
-include $(APP_DEPS)

# 冗余控制
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
