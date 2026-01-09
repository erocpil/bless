# Top-level Makefile
#
# ============================================================
# 全局参数
# ============================================================
BUILD    ?= release
STATIC   ?= 1
V        ?= 0
PREFIX   ?= /opt/bless-1.0
DESTDIR  ?=

# 根据 STATIC 值确定子目录名称：release-shared 或 release-static
BUILD_MODE := $(if $(filter 1,$(STATIC)),static,shared)
# 默认构建根目录
O          ?= build/$(BUILD)-$(BUILD_MODE)

# 转换为绝对路径，防止递归调用时路径漂移
TOPDIR     := $(CURDIR)
ABS_O      := $(abspath $(O))
SRCDIR     := src

# ============================================================
# 递归调用定义
# ============================================================
MAKE_SRC := $(MAKE) -C $(SRCDIR) \
            O="$(ABS_O)" \
            BUILD="$(BUILD)" \
            STATIC="$(STATIC)" \
            V="$(V)" \
            PREFIX="$(PREFIX)" \
            DESTDIR="$(DESTDIR)"

.PHONY: all clean install uninstall help

all:
	$(Q)$(MAKE_SRC)

install:
	$(Q)$(MAKE_SRC) install

uninstall:
	$(Q)$(MAKE_SRC) uninstall

clean:
	@echo "  CLEANING build directory..."
	$(Q)rm -rf build/

help:
	@echo "Usage: make [target] STATIC=[0|1] BUILD=[release|debug]"
	@echo "Example: make install STATIC=1  # 安装全静态版本"
