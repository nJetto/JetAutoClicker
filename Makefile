CC       ?= gcc
CFLAGS   ?= -O2 -Wall -Wextra
PREFIX   ?= /usr/local
VERSION  ?= 1.1.0

APP_CFLAGS := -DAPP_VERSION=\"$(VERSION)\"

PKG_CONFIG      ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner

GTK_CFLAGS     := $(shell $(PKG_CONFIG) --cflags gtk+-3.0 gio-2.0)
GTK_LIBS       := $(shell $(PKG_CONFIG) --libs gtk+-3.0 gio-2.0)
WAYLAND_CFLAGS := $(shell $(PKG_CONFIG) --cflags wayland-client)
WAYLAND_LIBS   := $(shell $(PKG_CONFIG) --libs wayland-client)

# The wlr-virtual-pointer protocol is bundled so that the build does not
# depend on the wlr-protocols package being installed.  wayland-scanner is
# part of the wayland package on Arch (libwayland-dev on Debian/Ubuntu).
PROTO_XML := protocols/wlr-virtual-pointer-unstable-v1.xml
PROTO_H   := protocols/wlr-virtual-pointer-unstable-v1-client-protocol.h
PROTO_C   := protocols/wlr-virtual-pointer-unstable-v1-protocol.c

jetautoclicker: main.c engine.c engine.h portal.c portal.h wayland.c wayland.h $(PROTO_H) $(PROTO_C)
	$(CC) $(CFLAGS) $(APP_CFLAGS) $(GTK_CFLAGS) $(WAYLAND_CFLAGS) -o $@ main.c engine.c portal.c wayland.c $(PROTO_C) $(GTK_LIBS) $(WAYLAND_LIBS) -lpthread

$(PROTO_H): $(PROTO_XML)
	@command -v $(WAYLAND_SCANNER) >/dev/null || { \
	    echo "wayland-scanner not found. Install 'wayland' (Arch) or 'libwayland-dev' (Debian)."; \
	    exit 1; }
	$(WAYLAND_SCANNER) client-header $< $@

$(PROTO_C): $(PROTO_XML)
	@command -v $(WAYLAND_SCANNER) >/dev/null || { \
	    echo "wayland-scanner not found. Install 'wayland' (Arch) or 'libwayland-dev' (Debian)."; \
	    exit 1; }
	$(WAYLAND_SCANNER) private-code $< $@

# Timing smoke test (writes to /dev/null instead of uinput, needs no GTK)
test: engine.c engine.h test_engine.c
	$(CC) $(CFLAGS) -DENGINE_TEST -o test_engine engine.c test_engine.c -lpthread
	./test_engine

# Checks that the wlr-virtual-pointer backend can connect to the running
# compositor (does not click anything).
test-wayland: test_wayland.c wayland.c wayland.h $(PROTO_H) $(PROTO_C)
	$(CC) $(CFLAGS) $(WAYLAND_CFLAGS) -o test_wayland test_wayland.c wayland.c $(PROTO_C) $(WAYLAND_LIBS) -lpthread
	./test_wayland

install: jetautoclicker
	install -Dm755 jetautoclicker $(DESTDIR)$(PREFIX)/bin/jetautoclicker
	install -Dm644 jetautoclicker.desktop $(DESTDIR)$(PREFIX)/share/applications/jetautoclicker.desktop
	install -Dm644 jetautoclicker.svg $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/jetautoclicker.svg
	install -Dm644 99-jetautoclicker.rules $(DESTDIR)$(PREFIX)/lib/udev/rules.d/99-jetautoclicker.rules
	install -Dm644 99-jetautoclicker.modules $(DESTDIR)$(PREFIX)/lib/modules-load.d/uinput.conf

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/jetautoclicker
	rm -f $(DESTDIR)$(PREFIX)/share/applications/jetautoclicker.desktop
	rm -f $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/jetautoclicker.svg
	rm -f $(DESTDIR)$(PREFIX)/lib/udev/rules.d/99-jetautoclicker.rules
	rm -f $(DESTDIR)$(PREFIX)/lib/modules-load.d/uinput.conf

# Build a .deb (installs into /usr, adds a JetAutoClicker entry to the app menu)
ARCH    := $(shell dpkg --print-architecture 2>/dev/null)
DEBDIR  := build/jetautoclicker_$(VERSION)_$(ARCH)

deb: jetautoclicker
	rm -rf $(DEBDIR)
	$(MAKE) install DESTDIR=$(CURDIR)/$(DEBDIR) PREFIX=/usr
	strip --strip-unneeded $(DEBDIR)/usr/bin/jetautoclicker
	install -d $(DEBDIR)/DEBIAN
	sed -e 's/@VERSION@/$(VERSION)/' -e 's/@ARCH@/$(ARCH)/' debian-control.in > $(DEBDIR)/DEBIAN/control
	install -m755 debian-postinst $(DEBDIR)/DEBIAN/postinst
	install -m755 debian-postrm $(DEBDIR)/DEBIAN/postrm
	dpkg-deb --root-owner-group --build $(DEBDIR) build/jetautoclicker_$(VERSION)_$(ARCH).deb
	@echo "Built build/jetautoclicker_$(VERSION)_$(ARCH).deb"

# Source tarball for the Arch PKGBUILD (makepkg expects it next to PKGBUILD).
dist:
	rm -rf build/dist
	mkdir -p build/dist/jetautoclicker-$(VERSION)
	tar -cf - --exclude=./build --exclude=./pkg --exclude=./src --exclude=./.git \
	    --exclude=./jetautoclicker --exclude=./test_engine --exclude=./test_wayland \
	    --exclude=./protocols/wlr-virtual-pointer-unstable-v1-client-protocol.h \
	    --exclude=./protocols/wlr-virtual-pointer-unstable-v1-protocol.c \
	    --exclude=./*.pkg.tar.zst --exclude=./*.tar.gz --exclude=./*.deb . | \
	    tar -xf - -C build/dist/jetautoclicker-$(VERSION)
	tar -C build/dist -czf build/jetautoclicker-$(VERSION).tar.gz jetautoclicker-$(VERSION)
	@echo "Built build/jetautoclicker-$(VERSION).tar.gz"
	@echo "Next: cp build/jetautoclicker-$(VERSION).tar.gz . && makepkg -si"

# One-time permission setup (needs sudo). Log out and back in afterwards.
# On Arch this is what enables the evdev global hotkey and the uinput fallback.
setup-permissions:
	@test -n "$(SUDO_USER)" || { echo "Run this target with sudo: sudo make setup-permissions"; exit 1; }
	install -Dm644 99-jetautoclicker.rules /etc/udev/rules.d/99-jetautoclicker.rules
	install -Dm644 99-jetautoclicker.modules /etc/modules-load.d/uinput.conf
	-modprobe uinput
	usermod -aG input $(SUDO_USER)
	udevadm control --reload
	udevadm trigger
	@echo "Done. Log out and back in (or reboot) so the new 'input' group membership applies."

clean:
	rm -rf jetautoclicker test_engine test_wayland build $(PROTO_H) $(PROTO_C)

.PHONY: test test-wayland install uninstall deb dist setup-permissions clean
