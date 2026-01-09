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

# ============================================================
# 第三方库与对象
# ============================================================
CIVET_DIR := 3rd/civetweb
CIVET_SRC := $(CIVET_DIR)/src/civetweb.c
CIVET_INC := $(CIVET_DIR)/include

CJSON_DIR := 3rd/cJSON
CJSON_SRC := $(CJSON_DIR)/cJSON.c
CJSON_INC := $(CJSON_DIR)

APP_SRC   := $(wildcard *.c)
APP_OBJ   := $(APP_SRC:%.c=$(OBJDIR)/%.o)
CIVET_OBJ := $(OBJDIR)/$(CIVET_SRC:.c=.o)
CJSON_OBJ := $(OBJDIR)/$(CJSON_SRC:.c=.o)

ALL_OBJ   := $(APP_OBJ) $(CIVET_OBJ) $(CJSON_OBJ)
DEP       := $(ALL_OBJ:.o=.d)

# ============================================================
# 编译链接逻辑
# ============================================================
DPDK_CFLAGS  := $(shell $(PKGCONF) --cflags libdpdk 2>/dev/null)
DPDK_LDFLAGS := $(shell $(PKGCONF) $(if $(filter 1,$(STATIC)),--static) --libs libdpdk 2>/dev/null)
YAML_LIBS    := $(shell $(PKGCONF) --libs forage-yaml 2>/dev/null || echo "-lyaml")
CRYPTO_LIBS  := $(shell $(PKGCONF) --libs libcrypto 2>/dev/null || echo "-lcrypto")

CFLAGS += -std=c11 -I../include -I$(CIVET_INC) -I$(CJSON_INC) -Wall \
		  -DALLOW_EXPERIMENTAL_API -DNO_SSL -DNO_CGI -DNO_FILESYSTEM -DUSE_WEBSOCKET \
		  $(DPDK_CFLAGS)

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
INTERNAL_LIBS := $(LIBDIR)/libcivetweb.a $(LIBDIR)/libcjson.a
LIB_EXT       := a
BUILD_SHARED  := 0
else
INTERNAL_LIBS := -L$(LIBDIR) -lcivetweb -lcjson
LIB_EXT       := so
BUILD_SHARED  := 1
LIB_CFLAGS    := -fPIC
endif

CIVET_LIB := $(LIBDIR)/libcivetweb.$(LIB_EXT)
CJSON_LIB := $(LIBDIR)/libcjson.$(LIB_EXT)

# ============================================================
# 构建规则
# ============================================================
.PHONY: all clean install uninstall libcivetweb libcjson

all: $(TARGET)

# 【核心改进】TARGET 依赖 CONFIG_TRACKER
# 如果 STATIC 改变，CONFIG_TRACKER 会更新，导致 TARGET 重新链接
$(TARGET): $(APP_OBJ) $(CIVET_LIB) $(CJSON_LIB) $(CONFIG_TRACKER)
	@mkdir -p $(@D)
	$(ECHO) "  LD [$(if $(filter 1,$(STATIC)),STATIC,SHARED)] $@"
	$(Q)$(CC) $(APP_OBJ) -o $@ \
		$(if $(filter 0,$(STATIC)),$(RPATH_FLAGS)) \
		$(INTERNAL_LIBS) \
		$(LDFLAGS) $(APP_LDFLAGS) $(DPDK_LDFLAGS) $(YAML_LIBS) $(CRYPTO_LIBS)

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

# 库构建模板
CIVET_REAL := libcivetweb.so.1.0.0
CIVET_SONAME := libcivetweb.so.1
CIVET_LINK := libcivetweb.so
CJSON_REAL := libcjson.so.1.0.0
CJSON_SONAME := libcjson.so.1
CJSON_LINK := libcjson.so

define LIB_RULE_TEMPLATE
$(LIBDIR)/lib$(1).a: $(2)
	@mkdir -p $$(@D)
	$$(ECHO) "  AR      $$@"
	$$(Q)rm -f $$@ && ar rcs $$@ $$^

$(LIBDIR)/$(5): $(LIBDIR)/$(4)
$(LIBDIR)/$(4): $(2)
	@mkdir -p $$(@D)
	$$(ECHO) "  LD      $$@"
	$$(Q)$$(CC) -shared -Wl,-soname,$(3) $$^ -o $$@
	$$(Q)ln -sf $(4) $(LIBDIR)/$(3)
	$$(Q)ln -sf $(3) $(LIBDIR)/$(5)
endef

$(eval $(call LIB_RULE_TEMPLATE,civetweb,$(CIVET_OBJ),$(CIVET_SONAME),$(CIVET_REAL),$(CIVET_LINK)))
$(eval $(call LIB_RULE_TEMPLATE,cjson,$(CJSON_OBJ),$(CJSON_SONAME),$(CJSON_REAL),$(CJSON_LINK)))

libcivetweb: $(LIBDIR)/$(CIVET_REAL)
libcjson: $(LIBDIR)/$(CJSON_REAL)

$(OBJDIR)/%.o: %.c
	@mkdir -p $(@D)
	$(ECHO) "  CC      $<"
	$(Q)$(CC) $(CFLAGS) $(APP_CFLAGS) -MMD -MP -c $< -o $@

$(CIVET_OBJ) $(CJSON_OBJ): CFLAGS += $(LIB_CFLAGS)

# ============================================================
# 安装与卸载逻辑
# ============================================================
install: all
	$(ECHO) "  INSTALL to $(DESTDIR)$(PREFIX)"
	$(Q)install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/include/$(APP)
	$(Q)install -m 0755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/
ifeq ($(STATIC),1)
	$(Q)install -m 0644 $(LIBDIR)/*.a $(DESTDIR)$(PREFIX)/lib/
else
	$(Q)install -m 0755 $(LIBDIR)/$(CIVET_REAL) $(DESTDIR)$(PREFIX)/lib/
	$(Q)ln -sf $(CIVET_REAL) $(DESTDIR)$(PREFIX)/lib/$(CIVET_SONAME)
	$(Q)ln -sf $(CIVET_SONAME) $(DESTDIR)$(PREFIX)/lib/$(CIVET_LINK)
	$(Q)install -m 0755 $(LIBDIR)/$(CJSON_REAL) $(DESTDIR)$(PREFIX)/lib/
	$(Q)ln -sf $(CJSON_REAL) $(DESTDIR)$(PREFIX)/lib/$(CJSON_SONAME)
	$(Q)ln -sf $(CJSON_SONAME) $(DESTDIR)$(PREFIX)/lib/$(CJSON_LINK)
endif
	$(Q)install -m 0644 $(CIVET_INC)/*.h $(CJSON_INC)/*.h $(DESTDIR)$(PREFIX)/include/$(APP)/

uninstall:
	$(Q)rm -f $(DESTDIR)$(PREFIX)/bin/$(APP)
	$(Q)rm -f $(DESTDIR)$(PREFIX)/lib/libcivetweb.* $(DESTDIR)$(PREFIX)/lib/libcjson.*
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

-include $(DEP)
