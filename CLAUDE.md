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
