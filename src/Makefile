# SPDX-License-Identifier: BSD-3-Clause

APP     := bless
O       ?= build/$(BUILD)
BUILD   ?= release
STATIC  ?= 0
V       ?= 0

CC      := clang
PKGCONF := pkg-config

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
TARGET := $(BINDIR)/$(APP)

CIVET_DIR := 3rd/civetweb
CIVET_SRC := $(CIVET_DIR)/src/civetweb.c
CIVET_INC := $(CIVET_DIR)/include

CJSON_DIR := 3rd/cJSON
CJSON_SRC := $(CJSON_DIR)/cJSON.c
CJSON_INC  := $(CJSON_DIR)

# ============================================================
# Sources and objects
# ============================================================
SRC := $(wildcard *.c) $(CIVET_SRC) $(CJSON_SRC)
OBJ := $(SRC:%.c=$(OBJDIR)/%.o)
DEP := $(OBJ:.o=.d)

# ============================================================
# Flags
# ============================================================
DPDK_CFLAGS := $(shell $(PKGCONF) --cflags libdpdk 2>/dev/null)
DPDK_LDFLAGS := $(shell $(PKGCONF) $(if $(filter 1,$(STATIC)),--static) --libs libdpdk 2>/dev/null)

CFLAGS := -I../include -I$(CIVET_INC) -I$(CJSON_INC) -Wall \
          -DALLOW_EXPERIMENTAL_API \
          -DNO_SSL -DNO_CGI -DNO_FILESYSTEM -DUSE_WEBSOCKET \
          $(DPDK_CFLAGS)

ifeq ($(BUILD),debug)
CFLAGS += -O0 -g -DDEBUG
else
CFLAGS += -O2 -DNDEBUG
endif

LDFLAGS := $(DPDK_LDFLAGS) -lyaml -lcrypto

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
# Targets
# ============================================================
.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJ)
	@mkdir -p $(@D)
	$(ECHO) "  LD      $@"
	$(Q)$(CC) $(OBJ) -o $@ $(LDFLAGS)
	$(Q)ln -sf bin/$(APP) $(O)/$(APP)

$(OBJDIR)/%.o: %.c
	@mkdir -p $(@D)
	$(ECHO) "  CC      $<"
	$(Q)$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

clean:
	$(Q)rm -rf $(O)

-include $(DEP)
