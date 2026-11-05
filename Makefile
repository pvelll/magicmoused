CC         ?= gcc
CFLAGS     ?= -O2 -g

# Version: the git tag (v1.2.3 -> 1.2.3), or pass VERSION=... explicitly.
VERSION    ?= $(shell v=$$(git describe --tags --always --dirty 2>/dev/null) && echo $${v\#v} || echo dev)
DEB_ARCH   ?= $(shell dpkg --print-architecture 2>/dev/null || echo amd64)

# Always applied, even when CFLAGS is given on the command line.
BUILD_FLAGS = -std=gnu11 -Wall -Wextra -Wshadow -Wno-unused-parameter -DMM_VERSION='"$(VERSION)"'
COMPILE     = $(CC) $(BUILD_FLAGS) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS)

PREFIX     ?= /usr/local
BINDIR     ?= $(PREFIX)/bin
SYSCONFDIR ?= /etc
UNITDIR    ?= /etc/systemd/system

CORE    = src/config.c src/report.c src/engine.c
HEADERS = src/mm.h src/device.h src/vinput.h

.PHONY: all test install uninstall deb clean

all: magicmoused magicmouse-dump

magicmoused: $(CORE) src/device.c src/vinput.c src/main.c $(HEADERS)
	$(COMPILE) -o $@ $(CORE) src/device.c src/vinput.c src/main.c -lm $(LDLIBS)

magicmouse-dump: $(CORE) src/device.c src/dump.c $(HEADERS)
	$(COMPILE) -o $@ $(CORE) src/device.c src/dump.c -lm $(LDLIBS)

# Engine tests: plain C, no hardware needed (also build on macOS).
tests/test_engine: $(CORE) tests/test_engine.c src/mm.h
	$(COMPILE) -Isrc -o $@ $(CORE) tests/test_engine.c -lm $(LDLIBS)

test: tests/test_engine
	./tests/test_engine

install: magicmoused magicmouse-dump
	install -Dm755 magicmoused $(DESTDIR)$(BINDIR)/magicmoused
	install -Dm755 magicmouse-dump $(DESTDIR)$(BINDIR)/magicmouse-dump
	@if [ -e $(DESTDIR)$(SYSCONFDIR)/magicmouse/magicmouse.conf ]; then \
		install -Dm644 magicmouse.conf $(DESTDIR)$(SYSCONFDIR)/magicmouse/magicmouse.conf.new; \
		echo "Kept the existing $(SYSCONFDIR)/magicmouse/magicmouse.conf (new defaults: magicmouse.conf.new)"; \
	else \
		install -Dm644 magicmouse.conf $(DESTDIR)$(SYSCONFDIR)/magicmouse/magicmouse.conf; \
	fi
	sed 's|@BINDIR@|$(BINDIR)|g' magicmouse.service > magicmouse.service.tmp
	install -Dm644 magicmouse.service.tmp $(DESTDIR)$(UNITDIR)/magicmouse.service
	rm -f magicmouse.service.tmp
	@echo
	@echo "Next: sudo systemctl daemon-reload && sudo systemctl enable --now magicmouse"

# Debian/Ubuntu package in dist/. Cross build: make deb DEB_ARCH=arm64 CC=aarch64-linux-gnu-gcc
deb: magicmoused magicmouse-dump
	packaging/build-deb.sh "$(VERSION)" "$(DEB_ARCH)" dist

uninstall:
	-systemctl disable --now magicmouse 2>/dev/null
	rm -f $(DESTDIR)$(BINDIR)/magicmoused $(DESTDIR)$(BINDIR)/magicmouse-dump
	rm -f $(DESTDIR)$(UNITDIR)/magicmouse.service
	@echo "Config left in $(SYSCONFDIR)/magicmouse/"

clean:
	rm -rf magicmoused magicmouse-dump tests/test_engine *.dSYM tests/*.dSYM magicmouse.service.tmp dist
