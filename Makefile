# ============================================================
# Top-level Makefile
# ============================================================

# ----------------------------
# 全局参数（可被命令行覆盖）
# ----------------------------
BUILD    ?= release
STATIC   ?= 1
V        ?= 0
PREFIX   ?= /opt/bless-1.0
DESTDIR  ?=

# ----------------------------
# 构建模式
# ----------------------------
BUILD_MODE := $(if $(filter 1,$(STATIC)),static,shared)
O          ?= build/$(BUILD)-$(BUILD_MODE)

# ----------------------------
# 路径定义
# ----------------------------
TOPDIR  := $(CURDIR)
ROOT    := $(abspath .)
ABS_O   := $(abspath $(O))
SRCDIR  := src

# third_party 导出的 mk（由 Makefile.up / third_party/Makefile 生成）
TP_MK   := $(ROOT)/third_party/third_party.mk

# ----------------------------
# 子 make 调用（src）
# ----------------------------
MAKE_SRC := $(MAKE) -C $(SRCDIR) \
			O="$(ABS_O)" \
			BUILD="$(BUILD)" \
			STATIC="$(STATIC)" \
			V="$(V)" \
			PREFIX="$(PREFIX)" \
			DESTDIR="$(DESTDIR)"

.DEFAULT_GOAL := all

# ============================================================
# third_party.mk 生成规则
# ============================================================

# 明确：third_party.mk 是一个“生成文件”
# 不存在时，通过 Makefile.up 生成
$(TP_MK): update-third-party
	$(MAKE) -f Makefile.up

update-third-party:
	$(MAKE) -f Makefile.up update-third-party

# ============================================================
# include third_party 输出（关键点）
# ============================================================

# 只有在非 clean 目标时，才 include
# 这里使用 *强 include*
# 因为我们已经告诉 make 如何生成它
ifneq ($(filter clean distclean help,$(MAKECMDGOALS)),clean distclean help)
include $(TP_MK)
endif

# ============================================================
# 导出第三方变量给子目录（src）
# ============================================================

export CFLAGS  := $(CFLAGS)  $(THIRD_PARTY_CFLAGS)
export LDFLAGS := $(LDFLAGS) $(THIRD_PARTY_LDFLAGS)
export LDLIBS  := $(LDLIBS)  $(THIRD_PARTY_LDLIBS)

# ============================================================
# 目标定义
# ============================================================

.PHONY: all upstream install uninstall clean distclean help

# 默认目标
all: $(TP_MK)
	$(Q)$(MAKE_SRC)

# 仅准备 third_party（CI / 调试时很有用）
upstream: $(TP_MK)

install: $(TP_MK)
	$(Q)$(MAKE_SRC) install

uninstall:
	$(Q)$(MAKE_SRC) uninstall

clean:
	@echo "  CLEANING build directory..."
	$(Q)rm -rf build/

distclean: clean
	$(MAKE) -f Makefile.up clean
	$(MAKE) -C src clean

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
	@echo "  BUILD=release|debug"
	@echo "  PREFIX=/path  Install prefix"
