# Makefile - lwext4fs-mac (macFUSE FUSE2 front-end over lwext4)
#
# Feasibility scaffold. Build:
#     make
# Produces ./lwext4fs-mac

LWE      := /Users/ceco/lwext4-spike/lwext4
LWE_INC  := -I$(LWE)/include -I$(LWE)/build_generic/include -I$(LWE)/blockdev/linux
LWE_LIBS := $(LWE)/build_generic/src/liblwext4.a \
            $(LWE)/build_generic/blockdev/libblockdev.a

PKGCONF  := pkg-config
FUSE_CFLAGS := $(shell $(PKGCONF) --cflags fuse)
FUSE_LIBS   := $(shell $(PKGCONF) --libs fuse)

CC       ?= cc
CFLAGS   ?= -O2 -g
CFLAGS   += -Wall -Wextra $(LWE_INC) $(FUSE_CFLAGS)
LDFLAGS  += $(FUSE_LIBS) -lpthread

OBJS := src/main.o src/ops.o
BIN  := lwext4fs-mac

.PHONY: all clean
all: $(BIN)

src/%.o: src/%.c src/ops.h
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN): $(OBJS)
	$(CC) $(OBJS) $(LWE_LIBS) $(LDFLAGS) -o $@

clean:
	rm -f $(OBJS) $(BIN)
