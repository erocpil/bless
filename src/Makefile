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
# Library build options
# ============================================================
STATIC_LIB := $(if $(filter 1,$(STATIC)),a,so)

# ============================================================
# Targets
# ============================================================
.PHONY: all clean libcivetweb libcjson

# Main target
all: $(TARGET)

$(TARGET): $(OBJ) libcivetweb libcjson
	@mkdir -p $(@D)
	$(ECHO) "  LD      $@"
	$(Q)$(CC) $(OBJ) -o $@ $(LDFLAGS) -L$(O)/lib -lcivetweb -lcjson
	$(Q)ln -sf bin/$(APP) $(O)/$(APP)

# Library build rules
libcivetweb: $(O)/lib/libcivetweb.$(STATIC_LIB)
libcjson: $(O)/lib/libcjson.$(STATIC_LIB)

# Build civetweb as a static or dynamic library
$(O)/lib/libcivetweb.a: $(CIVET_SRC)
	@mkdir -p $(O)/lib
	$(ECHO) "  AR      $@"
	$(Q)$(CC) -c $(CIVET_SRC) -o $(OBJDIR)/civetweb.o $(CFLAGS)
	$(Q)ar rcs $@ $(OBJDIR)/civetweb.o

$(O)/lib/libcivetweb.so: $(CIVET_SRC)
	@mkdir -p $(O)/lib
	$(ECHO) "  CC      $@"
	$(Q)$(CC) -fPIC -shared $(CIVET_SRC) -o $@ $(CFLAGS)

# Build cJSON as a static or dynamic library
$(O)/lib/libcjson.a: $(CJSON_SRC)
	@mkdir -p $(O)/lib
	$(ECHO) "  AR      $@"
	$(Q)$(CC) -c $(CJSON_SRC) -o $(OBJDIR)/cjson.o $(CFLAGS)
	$(Q)ar rcs $@ $(OBJDIR)/cjson.o

$(O)/lib/libcjson.so: $(CJSON_SRC)
	@mkdir -p $(O)/lib
	$(ECHO) "  CC      $@"
	$(Q)$(CC) -fPIC -shared $(CJSON_SRC) -o $@ $(CFLAGS)

# General object rule
$(OBJDIR)/%.o: %.c
	@mkdir -p $(@D)
	$(ECHO) "  CC      $<"
	$(Q)$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Clean rule
clean:
	$(Q)rm -rf $(O)

-include $(DEP)
