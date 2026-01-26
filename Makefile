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
USE_VERSION_H  ?= 1
BL_VERSION ?= 1.0

# ----------------------------
# 构建模式
# ----------------------------
BUILD_MODE := $(if $(filter 1,$(STATIC)),static,shared)
O          ?= build/$(BUILD)-$(BUILD_MODE)

# ----------------------------
# 路径定义
# ----------------------------
TOPDIR   := $(CURDIR)
ROOT     := $(abspath .)
ABS_O    := $(abspath $(O))
SRCDIR   := src

# third_party 导出的 mk（由 Makefile.up / third_party/Makefile 生成）
TP_MK    := $(ROOT)/third_party/third_party.mk

# ----------------------------
# 子 make 调用（src）
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

# 版本信息头文件
VERSION_H = include/version.h

# ============================================================
# Clean 目标优先处理
# ============================================================
# 使用 ifneq 检查是否包含 clean/distclean/uninstall/help
ifneq ($(filter clean distclean uninstall help,$(MAKECMDGOALS)),)

.PHONY: clean distclean help
clean:
	@echo "  CLEANING build directory..."
	rm -rf build/

distclean: clean
	$(MAKE) -f Makefile.up clean
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
# 正常构建流程
# ============================================================

# ============================================================
# third_party.mk 生成规则
# ============================================================

# 当 third_party.mk 不存在时才生成（不依赖 update-third-party）
$(TP_MK):
	@echo "  GENERATING $@..."
	$(MAKE) -f Makefile.up

# 手动更新第三方库（独立目标）
.PHONY: update-third-party
update-third-party:
	$(MAKE) -f Makefile.up update-third-party
	$(MAKE) -f Makefile.up

# ============================================================
# include third_party 输出
# ============================================================
include $(TP_MK)

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
# 每次 make 都重新生成版本文件
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
# 导出第三方变量给子目录（src）
# ============================================================

export CFLAGS  := $(CFLAGS) $(THIRD_PARTY_CFLAGS)
export LDFLAGS := $(LDFLAGS) $(THIRD_PARTY_LDFLAGS)
export LDLIBS  := $(LDLIBS) $(THIRD_PARTY_LDLIBS)

# ============================================================
# 目标定义
# ============================================================

.PHONY: all upstream install uninstall $(VERSION_H) Test

# 默认目标
all: upstream $(VERSION_H)
	$(MAKE_SRC)

Test:
	$(MAKE_SRC) Test

# 仅准备 third_party（CI / 调试时很有用）
upstream: $(TP_MK)

install: $(TP_MK)
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
