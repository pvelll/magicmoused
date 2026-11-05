CC         ?= gcc
CFLAGS     ?= -O2 -g
CFLAGS     += -std=gnu11 -Wall -Wextra -Wshadow -Wno-unused-parameter
LDLIBS     += -lm

PREFIX     ?= /usr/local
BINDIR     ?= $(PREFIX)/bin
SYSCONFDIR ?= /etc
UNITDIR    ?= /etc/systemd/system

CORE    = src/config.c src/report.c src/engine.c
HEADERS = src/mm.h src/device.h src/vinput.h

.PHONY: all test install uninstall clean

all: magicmoused magicmouse-dump

magicmoused: $(CORE) src/device.c src/vinput.c src/main.c $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(CORE) src/device.c src/vinput.c src/main.c $(LDLIBS)

magicmouse-dump: $(CORE) src/device.c src/dump.c $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(CORE) src/device.c src/dump.c $(LDLIBS)

# Engine tests: plain C, no hardware needed (also build on macOS).
tests/test_engine: $(CORE) tests/test_engine.c src/mm.h
	$(CC) $(CFLAGS) -Isrc -o $@ $(CORE) tests/test_engine.c $(LDLIBS)

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

uninstall:
	-systemctl disable --now magicmouse 2>/dev/null
	rm -f $(DESTDIR)$(BINDIR)/magicmoused $(DESTDIR)$(BINDIR)/magicmouse-dump
	rm -f $(DESTDIR)$(UNITDIR)/magicmouse.service
	@echo "Config left in $(SYSCONFDIR)/magicmouse/"

clean:
	rm -rf magicmoused magicmouse-dump tests/test_engine tests/test_engine.dSYM magicmouse.service.tmp
