# songviz — Makefile

CC      ?= cc
CFLAGS  ?= -O2 -Wall
PREFIX  ?= /usr/local
BINDIR   = $(DESTDIR)$(PREFIX)/bin

UNAME := $(shell uname)
ifeq ($(UNAME),Darwin)
  FRAMEWORKS = -framework Accelerate -framework CoreText -framework CoreGraphics -framework CoreFoundation
  LDLIBS =
else
  # Linux: apt install libfftw3-dev  (titles require macOS CoreText)
  CFLAGS  += -DSONGVIZ_NO_CORETEXT
  LDLIBS   = -lfftw3f -lm
endif

all: src/render

src/render: src/render.c
	$(CC) $(CFLAGS) src/render.c -o $@ $(FRAMEWORKS) $(LDLIBS)

install: all
	install -d $(BINDIR)
	install -m 0755 bin/songviz $(BINDIR)/songviz
	install -m 0755 src/render $(BINDIR)/songviz-render
	@echo "Installed: $(BINDIR)/songviz"

uninstall:
	rm -f $(BINDIR)/songviz $(BINDIR)/songviz-render

test: all
	@bash test/run_tests.sh

clean:
	rm -f src/render

.PHONY: all install uninstall test clean
