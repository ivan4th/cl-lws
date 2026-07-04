# cl-lws - Common Lisp bindings for libwebsockets

## Overview

cl-lws provides Common Lisp bindings for the [libwebsockets](https://libwebsockets.org/) C library,
scoped to what the host application needs: raw TCP client/server
connections, an HTTP server, an MQTT client, timers, and fd watching,
all driven by a single-threaded lws service loop.

Built against the patched libwebsockets fork in `~/work/libwebsockets`
(MQTT RX parser fixes: fragmentation, CONNACK length, RETAIN flag on
received PUBLISHes), installed into `/usr/local`.

## Thread support is optional

32-bit armhf SBCL has no threads; cl-lws must keep working there.  The
`:lws-threads` feature (pushed in `package.lisp` on SB-THREAD/CCL)
gates all bordeaux-threads usage via `make-lws-lock`/`with-lws-lock`.
Never call bt2 functions directly outside that gate.

## Dependencies

- SBCL (2.5+)
- libwebsockets fork installed in /usr/local (`cmake -DLWS_ROLE_MQTT=ON`)
- Quicklisp with: alexandria, i4-diet-utils, iterate, cffi, babel,
  bordeaux-threads

## API sketch

```lisp
(ql:quickload :lws)
(lws:with-lws-context (ctx)
  ;; timers / deferral
  (lws:schedule ctx 1.5 #'thunk :repeat t)     ; -> timer, lws:cancel-timer
  (lws:defer ctx #'thunk)                      ; thread-safe wakeup
  ;; raw TCP: handler object implements lws:on-raw-* generics
  (lws:raw-connect ctx "host" 4242 handler)
  (lws:raw-listen ctx 0 handler :iface "127.0.0.1") ; port 0 = ephemeral
  ;; HTTP server: static mounts served by lws, dynamic requests
  ;; dispatched to the handler function as lws:http-request objects
  (lws:http-listen ctx 8080 #'handle-request
                   :mounts '(("/fe" "/path/to/dir" "index.html")))
  ;; MQTT client (callback-based; caller owns reconnect)
  (lws:mqtt-connect ctx :host "host" :client-id "me"
                        :on-established ... :on-message ...)
  (lws:run-context ctx))   ; blocking service loop; lws:stop-context
```

`(lws:set-log-level 7)` routes lws logs (ERR+WARN+NOTICE) to dbg;
levels are the usual LLL_* bitmask (255/4095 for more).

## Project Structure

- `lws.asd`, `lws.tests.asd` - ASDF systems (`(vtf:run-tests 'lws.tests)`)
- `package.lisp` - package + the `:lws-threads` feature
- `libwebsockets.lisp` - foreign library loading
- `grovel.lisp` - CFFI-Grovel structs/enums/constants (note: enum
  members can't be groveled with CONSTANT — use CENUM)
- `wrappers.lisp` - C shims for bit-field access
- `cffi.lisp` - all defcfuns
- `registry.lisp` - id<->object maps (ids cross the FFI as fake pointers)
- `context.lisp` - context/vhosts, run-context, defer, sul timers,
  `define-protocol-callback` (condition guards; unknown callback
  reasons tolerated)
- `raw.lisp` - raw TCP (one explicit vhost per listener; one lws_write
  per WRITEABLE, Lisp-side output queues)
- `fd.lisp` - fd readability watching (lws owns the fd - pass a dup)
- `http.lisp` - HTTP server ("cs-http" is the default protocol:
  lws dispatches plain HTTP to protocols[0])
- `mqtt.lisp` - MQTT client sessions (op queue, one in-flight ack op)

## Gotchas

- lws stores vhost strings (listen_accept_protocol etc.) without
  copying: allocate with context lifetime, never stack-scope them.
- MQTT connections migrate to a mux child wsi on ESTABLISHED; use the
  callback wsi, not the connect-time one.
- Mountpoints must not end in "/" ("/fe" matches "/fe" and "/fe/...",
  "/fe/" matches only "/fe/").
- lws_service ignores its timeout arg (since v3.2); the loop blocks
  until lws has something to do. Cross-thread wakeup = lws:defer
  (queue + lws_cancel_service).

## Modbus engine (csmb)

A small C library in `modbus/` (`libcsmodbus`) implementing a Modbus
master and slave over TCP or RTU (serial/fd), driven from Lisp through
CFFI (`modbus.lisp`).  The public contract is `modbus/csmb.h` — treat it
as the spec.

### Layout

- `csmb.h` — public API/enums (do not change the API; comments OK).
- `csmb-private.h` — internal structs shared across the C sources; kept
  free of `libwebsockets.h` so the pure-logic units build without lws.
- `csmb-codec.c` — PDU encode/decode, MBAP + RTU framing, CRC16 (pure).
- `csmb-sched.c` — master scheduler: span store, poll-program bunching,
  change detection, write FIFO, per-unit bookkeeping (pure).
- `csmb-image.c` — slave register image (pure).
- `csmb-event.c` — event batching/arena (pure).
- `csmb-transport.c` — tx queue, TCP listen/connect, serial open + fd
  adoption.
- `csmb-master.c` / `csmb-slave.c` — the lws-facing engines.
- `csmb-lws.c` — the single `"cs-modbus"` protocol callback.

### Build & test

- ASDF loads it via a custom `modbus-c-library` component that runs
  `make -C modbus` and loads the dylib; you rarely build it by hand.
- C unit tests: `make -C modbus clean all check` (event/codec/image/sched;
  `-Wall -Wextra -Werror -std=c99`, clean under ASan/UBSan).
- Fuzzers (coverage-guided, clang + libFuzzer only, not part of `check`):
  `make -C modbus fuzz` builds `tests/fuzz-{mbap,rtu,request}` (skips
  gracefully without libFuzzer).  Each also builds standalone under plain
  ASan (compile a harness without `-DCSMB_LIBFUZZER`) for toolchains
  lacking the libFuzzer runtime (e.g. Apple clang).
- Lisp loopback tests in `tests/modbus-test.lisp`
  (`test-modbus-slave-loopback`, `-master-loopback`, `-rtu-loopback` over
  an openpty pair): `(vtf:run-tests 'lws.tests)`.

### Event-batch drain contract

Engines batch events; the notify callback fires at most once per C-side
activation (lws callback / sul timer / a public API call that can emit
synchronously).  The consumer drains with `csmb_events_get()` (freezes
the batch) then `csmb_events_done()` (releases it); event payload pointers
(`values`) are valid only until `events_done`.  Calls made from inside the
notify land in the next batch.  In Lisp this is `%modbus-drain-events`,
fanning events out to the `on-modbus-*` handler generics.

**Everything is event-loop-thread-only** — no csmb API is thread-safe;
marshal cross-thread work through `lws:defer`.

### Transports

- `(:tcp HOST PORT)` master / `(:tcp-listen PORT :iface ...)` slave — MBAP
  framing, responses matched by transaction id.
- `(:serial DEVICE :baud .. :parity :none|:even|:odd :data-bits .. :stop-bits ..
  :t35 SECONDS)` — RTU framing (`csmb_rtu_wrap`, no tid; the master matches
  responses by unit + function code).  `csmb_serial_open` sets raw termios
  and tolerates `cfsetspeed` failures on ptys (logs + continues).
- `(:fd FD)` — adopt an already-open fd; the engine takes ownership and
  closes it.  A handed-over fd cannot be reopened, so on close the master
  goes permanently offline (`CSMB_CERR_CLOSED`); a serial *device* is
  reopened with the reconnect/backoff FSM.

### RTU notes

- **t3.5 idle gap**: a per-connection lws sul, re-armed on every RX chunk
  to `t35_us` (0 ⇒ derived from baud, floor 1750us); on fire it resets the
  RTU parser to resync after a partial/garbled frame.  `CSMB_PR_BAD` also
  resets the parser.  The master keeps a ≥ t3.5 gap between its own frames
  via the request-pacing mechanism.
- **RTU slave is multi-drop and silent**: it answers only units it serves
  (registered ranges), stays silent for others (no `GW_TARGET` — that is a
  TCP-gateway behavior), and never drops the shared bus.  Unit 0 is a
  broadcast: writes are applied (and surfaced as `SLAVE_WRITE`) with no
  reply; reads are ignored.
- **TCP dead-peer detection**: after 5 consecutive response timeouts with
  no valid frame in between, the master drops the connection
  (`CSMB_CERR_TIMEOUT`) and reconnects; the counter resets on any valid
  frame.
