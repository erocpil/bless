# SPDX-License-Identifier: BSD-3-Clause

APP     := bless
BUILD   ?= release
O       ?= build/$(BUILD)
STATIC  ?= 0
V       ?= 0
PKGCONF ?= pkg-config

ifeq ($(origin CC), default)

CLANG := $(shell command -v clang 2>/dev/null)
ifneq ($(CLANG),)
	CLANG_OK := $(shell echo 'int main(){}' | clang -x c - -o /dev/null 2>/dev/null && echo yes)
endif

ifeq ($(CLANG_OK),yes)
	CC := clang
else
	CC := gcc
endif
endif

# ============================================================
# Disable builtin rules
# ============================================================
MAKEFLAGS += -rR
.SUFFIXES:

# ============================================================
# Paths
# ============================================================
OBJDIR := $(O)/obj
BINDIR := $(O)/bin
LIBDIR := $(O)/lib
TARGET := $(BINDIR)/$(APP)

# ============================================================
# Third-party libraries
# ============================================================
CIVET_DIR := 3rd/civetweb
CIVET_SRC := $(CIVET_DIR)/src/civetweb.c
CIVET_INC := $(CIVET_DIR)/include

CJSON_DIR := 3rd/cJSON
CJSON_SRC := $(CJSON_DIR)/cJSON.c
CJSON_INC := $(CJSON_DIR)

# ============================================================
# Sources / objects
# ============================================================
APP_SRC   := $(wildcard *.c)
APP_OBJ   := $(APP_SRC:%.c=$(OBJDIR)/%.o)

CIVET_OBJ := $(OBJDIR)/$(CIVET_SRC:.c=.o)
CJSON_OBJ := $(OBJDIR)/$(CJSON_SRC:.c=.o)

OBJ := $(APP_OBJ) $(CIVET_OBJ) $(CJSON_OBJ)
DEP := $(OBJ:.o=.d)

# ============================================================
# Flags
# ============================================================
DPDK_CFLAGS  := $(shell $(PKGCONF) --cflags libdpdk 2>/dev/null)
DPDK_LDFLAGS := $(shell $(PKGCONF) $(if $(filter 1,$(STATIC)),--static) --libs libdpdk 2>/dev/null)

CFLAGS += -std=c11 -I../include -I$(CIVET_INC) -I$(CJSON_INC) -Wall \
		  -DALLOW_EXPERIMENTAL_API \
		  -DNO_SSL -DNO_CGI -DNO_FILESYSTEM -DUSE_WEBSOCKET \
		  $(DPDK_CFLAGS)

LDFLAGS += -std=c11 $(DPDK_LDFLAGS) -lyaml -lcrypto

ifeq ($(BUILD),debug)
	CFLAGS += -O0 -g -DDEBUG
else
	CFLAGS += -O2 -DNDEBUG
endif

ifeq ($(CC),clang)
	APP_CFLAGS := -Weverything -Wno-padded
	ifeq ($(BUILD),debug)
	ifeq ($(STATIC),0)
	# NOTE:
	# When using ASan with DPDK, consider:
	# export ASAN_OPTIONS=detect_leaks=0
	APP_CFLAGS += -fsanitize=address,undefined
	LAPP_DFLAGS += -fsanitize=address,undefined
endif
	endif
else
	CFLAGS += -Wextra
endif

# ============================================================
# Verbose control
# ============================================================
ifeq ($(V),1)
	Q :=
	ECHO := @true
else
	Q := @
	ECHO := @echo
endif

# ============================================================
# Library build options
# ============================================================
STATIC_LIB := $(if $(filter 1,$(STATIC)),a,so)

# ============================================================
# Targets
# ============================================================
.PHONY: all clean libcivetweb libcjson

# ------------------------------------------------------------
# Main target
# ------------------------------------------------------------
all: $(TARGET)

# ------------------------------------------------------------
# Final binary
# ------------------------------------------------------------
$(TARGET): $(OBJ) libcivetweb libcjson
	@mkdir -p $(@D)
	$(ECHO) "  LD      $@"
	$(Q)$(CC) $(OBJ) -o $@ $(LDFLAGS) -L$(LIBDIR) -lcivetweb -lcjson
	$(Q)ln -sf bin/$(APP) $(O)/$(APP)

# ------------------------------------------------------------
# Libraries
# ------------------------------------------------------------
libcivetweb: $(LIBDIR)/libcivetweb.$(STATIC_LIB)
libcjson:    $(LIBDIR)/libcjson.$(STATIC_LIB)

# Build civetweb as a static or dynamic library
$(LIBDIR)/libcivetweb.a: $(CIVET_OBJ)
	@mkdir -p $(LIBDIR)
	$(ECHO) "  AR      $@"
	$(Q)ar rcs $@ $^

$(LIBDIR)/libcivetweb.so: $(CIVET_OBJ)
	@mkdir -p $(LIBDIR)
	$(ECHO) "  LD      $@"
	$(Q)$(CC) -shared -fPIC -Wl,-soname,libcivetweb.so $^ -o $@

# Build cJSON as a static or dynamic library
$(LIBDIR)/libcjson.a: $(CJSON_OBJ)
	@mkdir -p $(LIBDIR)
	$(ECHO) "  AR      $@"
	$(Q)ar rcs $@ $^

$(LIBDIR)/libcjson.so: $(CJSON_OBJ)
	@mkdir -p $(LIBDIR)
	$(ECHO) "  LD      $@"
	$(Q)$(CC) -shared -fPIC -Wl,-soname,libcjson.so $^ -o $@

# ------------------------------------------------------------
# Unified object build rule
# ------------------------------------------------------------
$(OBJDIR)/%.o: %.c
	@mkdir -p $(@D)
	$(ECHO) "  CC      $<"
	$(Q)$(CC) $(CFLAGS) $(APP_CFLAGS) -MMD -MP -c $< -o $@

# ------------------------------------------------------------
# Clean
# ------------------------------------------------------------
clean:
	$(Q)rm -rf $(O)

-include $(DEP)
