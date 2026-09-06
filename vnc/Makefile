UNAME := $(shell uname -s)

CFLAGS += -O2 -g -fPIC -Wall -Wextra -Werror -std=c99 -I. -I/usr/local/include -D_GNU_SOURCE
LDFLAGS += -L/usr/local/lib -lwebsockets -lz

ifeq ($(UNAME),Darwin)
  LIB = libcsvnc.dylib
  SHFLAGS = -dynamiclib -install_name @rpath/libcsvnc.dylib
else
  LIB = libcsvnc.so
  SHFLAGS = -shared
endif

# pure-logic objects: no libwebsockets dependency, linked into tests
PURE_OBJS = csvnc-rfb.o csvnc-pixfmt.o csvnc-encode.o csvnc-region.o csvnc-keys.o
# lws-facing objects
LWS_OBJS = csvnc-lws.o

OBJS = $(PURE_OBJS) $(LWS_OBJS)

TESTS = tests/test-rfb tests/test-pixfmt tests/test-encode tests/test-region tests/test-keys

# coverage-guided fuzz harnesses (clang + libFuzzer only; not run by check)
FUZZERS = tests/fuzz-handshake tests/fuzz-messages tests/fuzz-encode
FUZZ_CC ?= clang
FUZZ_FLAGS = -g -O1 -std=c99 -DCSVNC_LIBFUZZER \
             -fsanitize=fuzzer,address,undefined -I.
# standalone (no libFuzzer) build of the same harnesses under ASan/UBSan:
# make fuzz-standalone && ./tests/fuzz-handshake-sa 200000
SA_FLAGS = -g -O1 -std=c99 -fsanitize=address,undefined -I.
PURE_SRCS = $(PURE_OBJS:.o=.c)

all: $(LIB)

$(LIB): $(OBJS)
	$(CC) $(SHFLAGS) -o $@ $(OBJS) $(LDFLAGS)

$(OBJS): csvnc.h csvnc-private.h Makefile

tests/test-%: tests/test-%.c tests/test-util.h $(PURE_OBJS) csvnc.h csvnc-private.h
	$(CC) $(CFLAGS) -o $@ $< $(PURE_OBJS)

check: $(TESTS)
	@set -e; for t in $(TESTS); do echo "== $$t"; ./$$t; done; echo "csvnc: all C tests passed"

# Build the fuzz harnesses; skips gracefully if the clang has no libFuzzer.
# Run e.g. ./tests/fuzz-messages -max_len=512 -runs=2000000
fuzz:
	@if printf 'int LLVMFuzzerTestOneInput(const unsigned char*d,unsigned long s){(void)d;(void)s;return 0;}\n' \
	    | $(FUZZ_CC) -fsanitize=fuzzer -x c - -o /dev/null 2>/dev/null; then \
	  set -e; for f in $(FUZZERS); do \
	    echo "== $$f"; $(FUZZ_CC) $(FUZZ_FLAGS) -o $$f $$f.c $(PURE_SRCS); \
	  done; echo "csvnc: fuzzers built ($(FUZZERS))"; \
	else \
	  echo "fuzz: $(FUZZ_CC) has no libFuzzer runtime; skipping"; \
	fi

fuzz-standalone:
	@set -e; for f in $(FUZZERS); do \
	  echo "== $$f-sa"; $(FUZZ_CC) $(SA_FLAGS) -o $$f-sa $$f.c $(PURE_SRCS); \
	done; echo "csvnc: standalone fuzzers built"

clean:
	rm -f $(OBJS) $(LIB) $(TESTS) $(FUZZERS) $(FUZZERS:=-sa)
	rm -rf tests/*.dSYM

.PHONY: all check clean fuzz fuzz-standalone
