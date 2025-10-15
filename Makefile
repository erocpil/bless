SRCDIR := src
BUILDDIR := build

.PHONY: all clean
all:
	make -C  $(SRCDIR) \
		BUILDDIR="../$(BUILDDIR)"

clean:
	make -C $(SRCDIR) clean \
		BUILDDIR="../$(BUILDDIR)"
