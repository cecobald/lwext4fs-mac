# Makefile — lwext4fs-mac : an ext4 read/write filesystem for macOS
# Front-end: macFUSE (FUSE 2 high-level API) over the lwext4 library.
#
# Quick build (fetches + patches + builds lwext4, then this):
#     ./build.sh
# With an already-built lwext4 tree:
#     make LWEXT4=/path/to/lwext4
#
# Install the binary:
#     sudo make install            # -> /usr/local/bin/lwext4fs-mac

LWEXT4   ?= vendor/lwext4
PKGCONF  ?= pkg-config

LWE_INC  := -I$(LWEXT4)/include -I$(LWEXT4)/build_generic/include -I$(LWEXT4)/blockdev/linux
LWE_LIBS := $(LWEXT4)/build_generic/src/liblwext4.a \
            $(LWEXT4)/build_generic/blockdev/libblockdev.a

FUSE_CFLAGS := $(shell $(PKGCONF) --cflags fuse)
FUSE_LIBS   := $(shell $(PKGCONF) --libs fuse)

CC       ?= cc
CFLAGS   ?= -O2 -g
CFLAGS   += -Wall -Wextra $(LWE_INC) $(FUSE_CFLAGS)
LDFLAGS  += $(FUSE_LIBS) -lpthread

PREFIX   ?= /usr/local
BIN      := lwext4fs-mac
OBJS     := src/main.o src/ops.o

.PHONY: all clean install uninstall
all: $(BIN)

src/%.o: src/%.c src/ops.h
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN): $(OBJS)
	$(CC) $(OBJS) $(LWE_LIBS) $(LDFLAGS) -o $@

install: $(BIN)
	install -d "$(DESTDIR)$(PREFIX)/bin"
	install -m 0755 $(BIN) "$(DESTDIR)$(PREFIX)/bin/$(BIN)"
	install -m 0755 scripts/lwext4fs-umount "$(DESTDIR)$(PREFIX)/bin/lwext4fs-umount"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/$(BIN)"

clean:
	rm -f $(OBJS) $(BIN)
