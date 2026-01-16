# SPDX-License-Identifier: BSD-3-Clause
APP      := bless
BUILD    ?= release
O        ?= build/$(BUILD)-shared
STATIC   ?= 1
V        ?= 0
PREFIX   ?= $(shell pwd)
PKGCONF  ?= pkg-config

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
LIBDIR := $(O)/lib
TARGET := $(BINDIR)/$(APP)

# 配置追踪器文件：记录当前的编译模式
CONFIG_TRACKER := $(OBJDIR)/.build_config

APP_SRC   := $(wildcard *.c)
APP_OBJ   := $(APP_SRC:%.c=$(OBJDIR)/%.o)
# ============================================================
# 编译链接逻辑
# ============================================================
DPDK_CFLAGS  := $(shell $(PKGCONF) --cflags libdpdk 2>/dev/null) -DALLOW_EXPERIMENTAL_API
DPDK_LDFLAGS := $(shell $(PKGCONF) $(if $(filter 1,$(STATIC)),--static) --libs libdpdk 2>/dev/null)
CRYPTO_LIBS  := $(shell $(PKGCONF) --libs libcrypto 2>/dev/null || echo "-lcrypto")

CFLAGS += -std=c11 -I../include -Wall $(DPDK_CFLAGS)

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
RPATH_FLAGS := -Wl,-rpath,'$$ORIGIN/../lib:$(PREFIX)/lib'

# 【关键点】根据 STATIC 决定链接方式
ifeq ($(STATIC),1)
BUILD_SHARED  := 0
else
BUILD_SHARED  := 1
LIB_CFLAGS    := -fPIC
endif

# ============================================================
# 构建规则
# ============================================================
.PHONY: all clean install uninstall libcivetweb libcjson

all: $(TARGET)

# 【核心改进】TARGET 依赖 CONFIG_TRACKER
# 如果 STATIC 改变，CONFIG_TRACKER 会更新，导致 TARGET 重新链接
$(TARGET): $(APP_OBJ) $(CONFIG_TRACKER)
	$(ECHO) $(CFLAGS)
	$(ECHO) $(LDFLAGS)
	$(ECHO) $(LDLIBS)
	@mkdir -p $(@D)
	$(ECHO) "  LD [$(if $(filter 1,$(STATIC)),STATIC,SHARED)] $@"
	$(Q)$(CC) $(APP_OBJ) -o $@ \
		$(if $(filter 0,$(STATIC)),$(RPATH_FLAGS)) \
		$(LDFLAGS) $(APP_LDFLAGS) $(DPDK_LDFLAGS) $(CRYPTO_LIBS) $(LDLIBS)

# 配置追踪器：检测 STATIC 变量是否发生变化
$(CONFIG_TRACKER): FORCE
	@mkdir -p $(@D)
	@echo "STATIC=$(STATIC)" > $@.tmp
	@if [ ! -f $@ ] || [ "`cat $@`" != "`cat $@.tmp`" ]; then \
		cp $@.tmp $@; \
		echo "  CONF    Build mode changed to $(if $(filter 1,$(STATIC)),STATIC,SHARED)"; \
		fi
	@rm -f $@.tmp

.PHONY: FORCE

$(OBJDIR)/%.o: %.c
	$(ECHO) $(CFLAGS)
	$(ECHO) $(LDFLAGS)
	$(ECHO) $(LDLIBS)
	@mkdir -p $(@D)
	$(ECHO) "  CC      $<"
	$(Q)$(CC) $(CFLAGS) $(APP_CFLAGS) -MMD -MP -c $< -o $@

# ============================================================
# 安装与卸载逻辑
# ============================================================
install: all
	$(ECHO) "  INSTALL to $(DESTDIR)$(PREFIX)"
	$(Q)install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/include/$(APP)
	$(Q)install -m 0755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/
ifeq ($(STATIC),1)
	$(Q)install -m 0644 $(LIBDIR)/*.a $(DESTDIR)$(PREFIX)/lib/
endif

uninstall:
	$(Q)rm -f $(DESTDIR)$(PREFIX)/bin/$(APP)
	$(Q)rm -rf $(DESTDIR)$(PREFIX)/include/$(APP)

# 冗余控制
ifeq ($(V),1)
Q :=
ECHO := echo
else
Q := @
ECHO := @echo
endif

clean:
	$(Q)rm -rf $(O)
