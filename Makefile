SRCDIR   := src
O        ?= build/$(BUILD)
BUILD    ?= release
BUILDDIR := build

.PHONY: all clean
all:
	make -C  $(SRCDIR) \
		O="../$(O)"

.PHONY: clean

clean:
	rm -fr $(BUILDDIR)
	make -C $(SRCDIR) clean BUILDDIR="../$(BUILDDIR)"
