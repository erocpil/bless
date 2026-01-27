## add new submodule
# git submodule add https://github.com/xxx/yyy.git third_party/xxx/upstream
# git commit -m "add xxx submodule"
## CI / new environment
# git clone ...
# make update-third-party
# make
#
## update third party library
# make update-third-party
## WARN
# cd third_party/libyaml/upstream
# git fetch --tags
# git tag -l
# git checkout <tag-or-commit>
# cd -
# git add third_party/libyaml/upstream
# git commit -m "pin libyaml to <tag-or-commit>"

ROOT := $(abspath ..)
LIB_DIR := $(ROOT)/lib
UP_MK := third_party.mk

# ============================================================
# public variables
# ============================================================

THIRD_PARTY_LIBS :=
THIRD_PARTY_INCS :=

# ============================================================
# macro template
# ============================================================

# -------- autotools library --------
define AUTOTOOLS_LIB

# $(1): name
# $(2): upstream dir
# $(3): include subdir
# $(4): output lib name

$(1)_UPSTREAM := $(abspath $(2))
$(1)_BUILD    := $(1)/build
$(1)_STAMP    := $$($(1)_BUILD)/.configured
$(1)_LIB      := $(LIB_DIR)/$(4)
$(1)_INC      := $$($(1)_UPSTREAM)/$(3)

THIRD_PARTY_LIBS += $(4)
THIRD_PARTY_INCS += $$($(1)_INC)

update-$(1):
	git submodule update --init --recursive $$($(1)_UPSTREAM)

$$($(1)_STAMP): update-$(1)
	mkdir -p $$($(1)_BUILD)
	cd $$($(1)_UPSTREAM) && ./bootstrap
	cd $$($(1)_BUILD) && $$($(1)_UPSTREAM)/configure \
		--disable-shared --enable-static
	touch $$@

build-$(1): $$($(1)_LIB)

$$($(1)_LIB): $$($(1)_STAMP)
	$$(MAKE) -C $$($(1)_BUILD)
	mkdir -p $(LIB_DIR)
	cp $$($(1)_BUILD)/src/.libs/$(4) $$@

endef


# -------- cmake library --------
define CMAKE_LIB

# $(1): name
# $(2): upstream dir
# $(3): include subdir
# $(4): output lib name

$(1)_UPSTREAM := $(abspath $(2))
$(1)_BUILD    := $(1)/build
$(1)_STAMP    := $$($(1)_BUILD)/.configured
$(1)_LIB      := $(LIB_DIR)/$(4)
$(1)_INC      := $$($(1)_UPSTREAM)/$(3)

THIRD_PARTY_LIBS += $(4)
THIRD_PARTY_INCS += $$($(1)_INC)

update-$(1):
	git submodule update --init --recursive $$($(1)_UPSTREAM)

$$($(1)_STAMP): update-$(1)
	mkdir -p $$($(1)_BUILD)
	cd $$($(1)_BUILD) && cmake \
		-DBUILD_SHARED_LIBS=OFF \
		-DENABLE_TESTING=OFF \
		$$($(1)_UPSTREAM)
	touch $$@

build-$(1): $$($(1)_LIB)

$$($(1)_LIB): $$($(1)_STAMP)
	$$(MAKE) -C $$($(1)_BUILD)
	mkdir -p $(LIB_DIR)
	cp $$($(1)_BUILD)/$(4) $$@

endef


# -------- native make library --------
define MAKE_LIB

# $(1): name
# $(2): upstream dir
# $(3): include subdir
# $(4): output lib name

$(1)_UPSTREAM := $(abspath $(2))
$(1)_STAMP    := $(1)/build/.configured
$(1)_LIB      := $(LIB_DIR)/$(4)
$(1)_INC      := $$($(1)_UPSTREAM)/$(3)

# optional: private cflags
$(1)_CFLAGS ?=

THIRD_PARTY_LIBS += $(4)
THIRD_PARTY_INCS += $$($(1)_INC)

update-$(1):
	git submodule update --init --recursive $$($(1)_UPSTREAM)

$$($(1)_STAMP): update-$(1)
	mkdir -p $(1)/build
	touch $$@

build-$(1): $$($(1)_LIB)

$$($(1)_LIB): $$($(1)_STAMP)
	$$(MAKE) -C $$($(1)_UPSTREAM) \
		BUILD_SHARED_LIBS=0 \
		$$($(1)_CFLAGS) \
		lib
	mkdir -p $(LIB_DIR)
	cp $$($(1)_UPSTREAM)/$(4) $$@

endef

# ============================================================
# submodule tag check helpers
# ============================================================

define CHECK_TAG
check-$(1)-tag:
	@cd $$($(1)_UPSTREAM) && \
		if git describe --tags --exact-match >/dev/null 2>&1; then \
		echo "[OK] $(1) on tag $$(git describe --tags --exact-match)"; \
		else \
		echo "[WARN] $(1) not on a release tag"; \
		fi

endef

$(eval $(call CHECK_TAG,libyaml))
$(eval $(call CHECK_TAG,cjson))
$(eval $(call CHECK_TAG,civetweb))

# ============================================================
# use macro: declare third party libraries
# ============================================================

$(eval $(call AUTOTOOLS_LIB,libyaml,libyaml/upstream,include,libyaml.a))
$(eval $(call CMAKE_LIB,cjson,cjson/upstream,.,libcjson.a))
civetweb_CFLAGS := WITH_CFLAGS="-DNO_SSL -DNO_CGI -DNO_FILESYSTEM -DUSE_WEBSOCKET"
$(eval $(call MAKE_LIB,civetweb,civetweb/upstream,include,libcivetweb.a))

# ============================================================
# third_party.mk auto genaration
# ============================================================

$(UP_MK): \
	$(LIB_DIR)/libyaml.a \
	$(LIB_DIR)/libcjson.a \
	$(LIB_DIR)/libcivetweb.a
	@echo "# auto generated, do not edit" > $@
	@echo "THIRD_PARTY_CFLAGS := $(foreach I,$(THIRD_PARTY_INCS),-I$(I))" >> $@
	@echo "THIRD_PARTY_LDFLAGS := -L$(LIB_DIR)" >> $@
	@echo "THIRD_PARTY_LDLIBS := $(patsubst lib%.a,-l%,$(THIRD_PARTY_LIBS))" >> $@

# ============================================================
# top level targets
# ============================================================

.PHONY: all update build clean

all: build $(UP_MK)

update: update-libyaml update-cjson update-civetweb

build: build-libyaml build-cjson build-civetweb $(UP_MK)

clean:
	rm -f $(LIB_DIR)/lib*.a $(UP_MK)

.PHONY: update-third-party

update-third-party: \
	update-libyaml update-cjson update-civetweb versions \
	check-libyaml-tag check-cjson-tag check-civetweb-tag
	@echo "== third_party submodules are initialized and checked =="
	@echo "== to upgrade, checkout a tag inside submodule and commit =="

VERSIONS_MD := THIRD_PARTY_VERSIONS.md

UP_MODULES := libyaml cjson civetweb

.PHONY: versions

versions:
	@echo "# Third Party Versions" > $(VERSIONS_MD); \
		echo "" >> $(VERSIONS_MD); \
		echo "_Auto-generated. Do not edit manually._" >> $(VERSIONS_MD); \
		echo "" >> $(VERSIONS_MD); \
		echo "| Name | URL | Commit | Tag | Status |" >> $(VERSIONS_MD); \
		echo "|------|-----|--------|-----|--------|" >> $(VERSIONS_MD); \
		$(foreach m,$(UP_MODULES), \
		dir="$($(m)_UPSTREAM)"; \
		url=$$(git -C "$$dir" remote get-url origin); \
		commit=$$(git -C "$$dir" rev-parse --short HEAD); \
		tag=$$(git -C "$$dir" describe --tags --exact-match 2>/dev/null || true); \
		if [ -n "$$tag" ]; then status="OK"; else status="WARN"; tag=""; fi; \
		echo "| $(m) | $$url | $$commit | $$tag | $$status |" >> $(VERSIONS_MD); \
		)
