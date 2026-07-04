UNAME := $(shell uname -s)

CFLAGS += -O2 -g -fPIC -Wall -Wextra -Werror -std=c99 -I. -I/usr/local/include
LDFLAGS += -L/usr/local/lib -lwebsockets

ifeq ($(UNAME),Darwin)
  LIB = libcsmodbus.dylib
  SHFLAGS = -dynamiclib -install_name @rpath/libcsmodbus.dylib
  LIBUTIL =
else
  LIB = libcsmodbus.so
  SHFLAGS = -shared
  LIBUTIL = -lutil
endif

# pure-logic objects: no libwebsockets dependency, linked into tests
PURE_OBJS = csmb-event.o csmb-codec.o csmb-image.o csmb-sched.o
# lws-facing objects
LWS_OBJS = csmb-lws.o csmb-master.o csmb-slave.o csmb-transport.o csmb-misc.o

OBJS = $(PURE_OBJS) $(LWS_OBJS)

TESTS = tests/test-event tests/test-codec

all: $(LIB)

$(LIB): $(OBJS)
	$(CC) $(SHFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(LIBUTIL)

$(OBJS): csmb.h csmb-private.h Makefile

tests/test-%: tests/test-%.c tests/test-util.h $(PURE_OBJS) csmb.h csmb-private.h
	$(CC) $(CFLAGS) -o $@ $< $(PURE_OBJS)

check: $(TESTS)
	@set -e; for t in $(TESTS); do echo "== $$t"; ./$$t; done; echo "csmb: all C tests passed"

clean:
	rm -f $(OBJS) $(LIB) $(TESTS)

.PHONY: all check clean
