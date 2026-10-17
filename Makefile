CC       ?= gcc
CFLAGS   ?= -O2 -Wall -Wextra
PREFIX   ?= /usr/local

GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 gio-2.0)
GTK_LIBS   := $(shell pkg-config --libs gtk+-3.0 gio-2.0)

jetautoclicker: main.c engine.c engine.h portal.c portal.h
	$(CC) $(CFLAGS) $(GTK_CFLAGS) -o $@ main.c engine.c portal.c $(GTK_LIBS) -lpthread

# Timing smoke test (writes to /dev/null instead of uinput, needs no GTK)
test: engine.c engine.h test_engine.c
	$(CC) $(CFLAGS) -DENGINE_TEST -o test_engine engine.c test_engine.c -lpthread
	./test_engine

install: jetautoclicker
	install -Dm755 jetautoclicker $(DESTDIR)$(PREFIX)/bin/jetautoclicker
	install -Dm644 jetautoclicker.desktop $(DESTDIR)$(PREFIX)/share/applications/jetautoclicker.desktop
	install -Dm644 jetautoclicker.svg $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/jetautoclicker.svg

# Build a .deb (installs into /usr, adds a JetAutoClicker entry to the KDE/GNOME app menu)
VERSION ?= 1.0.0
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

# One-time permission setup (needs sudo). Log out and back in afterwards.
setup-permissions:
	install -Dm644 99-jetautoclicker.rules /etc/udev/rules.d/99-jetautoclicker.rules
	echo uinput > /etc/modules-load.d/uinput.conf
	modprobe uinput
	usermod -aG input $(SUDO_USER)
	udevadm control --reload
	udevadm trigger

clean:
	rm -rf jetautoclicker test_engine build

.PHONY: test install deb setup-permissions clean
