CC ?= cc
PKG_CONFIG ?= pkg-config
PKGS = libnm gtk+-3.0
CFLAGS ?= -O2 -Wall
PLUGIN = libnm-vpn-plugin-socks5.so

all: $(PLUGIN)

$(PLUGIN): properties/nm-socks5-editor.c
	$(CC) -shared -fPIC $(CFLAGS) `$(PKG_CONFIG) --cflags $(PKGS)` \
		-o $@ properties/nm-socks5-editor.c \
		`$(PKG_CONFIG) --libs $(PKGS)`

clean:
	rm -f $(PLUGIN)

.PHONY: all clean
