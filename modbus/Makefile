UNAME := $(shell uname -s)

CFLAGS += -O2 -g -fPIC -Wall -Wextra -Werror -std=c99 -I. -I/usr/local/include -D_GNU_SOURCE
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
LWS_OBJS = csmb-lws.o csmb-master.o csmb-slave.o csmb-transport.o csmb-misc.o csmb-lws-glue.o

OBJS = $(PURE_OBJS) $(LWS_OBJS)

TESTS = tests/test-event tests/test-codec tests/test-image tests/test-sched

# coverage-guided fuzz harnesses (clang + libFuzzer only; not run by check)
FUZZERS = tests/fuzz-mbap tests/fuzz-rtu tests/fuzz-request
FUZZ_CC ?= clang
FUZZ_FLAGS = -g -O1 -std=c99 -DCSMB_LIBFUZZER \
             -fsanitize=fuzzer,address,undefined -I.

all: $(LIB)

$(LIB): $(OBJS)
	$(CC) $(SHFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(LIBUTIL)

$(OBJS): csmb.h csmb-private.h Makefile

tests/test-%: tests/test-%.c tests/test-util.h $(PURE_OBJS) csmb.h csmb-private.h
	$(CC) $(CFLAGS) -o $@ $< $(PURE_OBJS)

check: $(TESTS)
	@set -e; for t in $(TESTS); do echo "== $$t"; ./$$t; done; echo "csmb: all C tests passed"

# Build the fuzz harnesses; skips gracefully if the clang has no libFuzzer.
# Run e.g. ./tests/fuzz-rtu -max_len=512 -runs=2000000
fuzz:
	@if printf 'int LLVMFuzzerTestOneInput(const unsigned char*d,unsigned long s){(void)d;(void)s;return 0;}\n' \
	    | $(FUZZ_CC) -fsanitize=fuzzer -x c - -o /dev/null 2>/dev/null; then \
	  set -e; for f in $(FUZZERS); do \
	    echo "== $$f"; $(FUZZ_CC) $(FUZZ_FLAGS) -o $$f $$f.c csmb-codec.c; \
	  done; echo "csmb: fuzzers built ($(FUZZERS))"; \
	else \
	  echo "fuzz: $(FUZZ_CC) has no libFuzzer runtime; skipping"; \
	fi

clean:
	rm -f $(OBJS) $(LIB) $(TESTS) $(FUZZERS)

.PHONY: all check clean fuzz
