UNAME := $(shell uname -s)

# zlib and libzstd: pkg-config where it knows them (Homebrew on macOS,
# the -dev packages on Debian), the plain library names otherwise
ZLIBS_CFLAGS := $(shell pkg-config --cflags zlib libzstd 2>/dev/null)
ZLIBS_LIBS := $(shell pkg-config --libs zlib libzstd 2>/dev/null || echo -lz -lzstd)

CFLAGS += -O2 -g -fPIC -Wall -Wextra -Werror -std=c99 -I. $(ZLIBS_CFLAGS)
LDFLAGS += $(ZLIBS_LIBS)

ifeq ($(UNAME),Darwin)
  LIB = libcscompress.dylib
  SHFLAGS = -dynamiclib -install_name @rpath/libcscompress.dylib
else
  LIB = libcscompress.so
  SHFLAGS = -shared
endif

OBJS = cscompress.o

TESTS = tests/test-compress

all: $(LIB)

$(LIB): $(OBJS)
	$(CC) $(SHFLAGS) -o $@ $(OBJS) $(LDFLAGS)

$(OBJS): cscompress.h Makefile

tests/test-%: tests/test-%.c tests/test-util.h $(OBJS) cscompress.h
	$(CC) $(CFLAGS) -I. -o $@ $< $(OBJS) $(LDFLAGS)

# the library too: CI builds and checks in one go, then installs it
check: $(LIB) $(TESTS)
	@set -e; for t in $(TESTS); do echo "== $$t"; ./$$t; done; echo "cscompress: all C tests passed"

clean:
	rm -f $(OBJS) $(LIB) $(TESTS)

.PHONY: all check clean
