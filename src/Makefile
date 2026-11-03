CC ?= gcc
GTK_CFLAGS := $(shell pkg-config --cflags gtk+-3.0)
GTK_LIBS := $(shell pkg-config --libs gtk+-3.0)

CFLAGS ?= -O2 -Wall -Wextra -pedantic $(GTK_CFLAGS)
LIBS ?= $(GTK_LIBS) -lpthread

TARGET = snescontrol
SRC = snescontrol.c

PKG_NAME = snescontrol
PKG_VER = 1.1.0
PKG_ARCH = amd64
DEB_NAME = $(PKG_NAME)_$(PKG_VER)_$(PKG_ARCH).deb
PKG_DIR = pkg_build

all: $(TARGET) icons

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

icons: snescontrol.png

snescontrol.png: OIP.Ffp9fIjabiJKp4y-JJAAEAHaHa.webp generate_icons.py
	python3 generate_icons.py

clean:
	rm -f $(TARGET) $(DEB_NAME)
	rm -rf $(PKG_DIR)

test: $(TARGET)
	./$(TARGET) --test

cli: $(TARGET)
	./$(TARGET) --cli

deb: $(TARGET) icons
	rm -rf $(PKG_DIR)
	mkdir -p $(PKG_DIR)/DEBIAN
	mkdir -p $(PKG_DIR)/usr/bin
	mkdir -p $(PKG_DIR)/usr/share/applications
	mkdir -p $(PKG_DIR)/usr/share/pixmaps
	mkdir -p $(PKG_DIR)/lib/udev/rules.d
	mkdir -p $(PKG_DIR)/usr/lib/systemd/user
	# Installer le binaire
	install -m 755 $(TARGET) $(PKG_DIR)/usr/bin/$(TARGET)
	# Raccourci desktop
	install -m 644 snescontrol.desktop $(PKG_DIR)/usr/share/applications/snescontrol.desktop
	# Pixmaps
	install -m 644 snescontrol.png $(PKG_DIR)/usr/share/pixmaps/snescontrol.png
	# Icones hicolor
	for s in 16 24 32 48 64 128 256 512; do \
		mkdir -p $(PKG_DIR)/usr/share/icons/hicolor/$${s}x$${s}/apps; \
		if [ -f icons/$${s}x$${s}/snescontrol.png ]; then \
			install -m 644 icons/$${s}x$${s}/snescontrol.png $(PKG_DIR)/usr/share/icons/hicolor/$${s}x$${s}/apps/snescontrol.png; \
		fi; \
	done
	# Règle udev
	install -m 644 70-snescontrol.rules $(PKG_DIR)/lib/udev/rules.d/70-snescontrol.rules
	# Service systemd utilisateur
	install -m 644 snescontrol.service $(PKG_DIR)/usr/lib/systemd/user/snescontrol.service
	# Fichier de contrôle Debian
	printf "Package: $(PKG_NAME)\n\
Version: $(PKG_VER)\n\
Section: utils\n\
Priority: optional\n\
Architecture: $(PKG_ARCH)\n\
Depends: libgtk-3-0, libc6\n\
Maintainer: Aristide <aristide@localhost>\n\
Description: Adaptateur et panneau de contrôle pour manette SNES USB\n\
 Corrige les signaux analogiques de la croix directionnelle (0810:e501)\n\
 et expose une manette virtuelle standard via uinput avec interface GTK3.\n" > $(PKG_DIR)/DEBIAN/control
	# Script postinst
	printf "#!/bin/sh\nset -e\n\
if command -v udevadm >/dev/null 2>&1; then udevadm control --reload-rules || true; udevadm trigger || true; fi\n\
if command -v update-desktop-database >/dev/null 2>&1; then update-desktop-database -q || true; fi\n\
if command -v gtk-update-icon-cache >/dev/null 2>&1; then gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true; fi\n\
exit 0\n" > $(PKG_DIR)/DEBIAN/postinst
	chmod 755 $(PKG_DIR)/DEBIAN/postinst
	# Script postrm
	printf "#!/bin/sh\nset -e\n\
if command -v udevadm >/dev/null 2>&1; then udevadm control --reload-rules || true; udevadm trigger || true; fi\n\
if command -v update-desktop-database >/dev/null 2>&1; then update-desktop-database -q || true; fi\n\
if command -v gtk-update-icon-cache >/dev/null 2>&1; then gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor || true; fi\n\
exit 0\n" > $(PKG_DIR)/DEBIAN/postrm
	chmod 755 $(PKG_DIR)/DEBIAN/postrm
	find $(PKG_DIR) -type d -exec chmod 755 {} +
	# Construction du paquet .deb
	dpkg-deb --build --root-owner-group $(PKG_DIR) $(DEB_NAME)
	rm -rf $(PKG_DIR)
	@echo "=========================================================="
	@echo " Paquet deb généré avec succès : $(DEB_NAME)"
	@echo " Installez-le avec : sudo apt install ./$(DEB_NAME)"
	@echo "=========================================================="

.PHONY: all icons clean test cli deb
