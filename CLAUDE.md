# cl-lws - Common Lisp bindings for libwebsockets

## Overview

cl-lws provides Common Lisp bindings for the [libwebsockets](https://libwebsockets.org/) C library,
with initial focus on MQTT client functionality.

## Dependencies

- SBCL (tested with 2.5.0)
- libwebsockets (installed in /usr/local)
- Quicklisp with: alexandria, iterate, cffi, babel, bordeaux-threads-2

## Usage

```lisp
;; Load the system
(ql:quickload :lws)

;; Run the MQTT subscriber demo (connects to localhost)
(lws:qqq)
```

## Project Structure

- `lws.asd` - ASDF system definition
- `package.lisp` - Package definition
- `libwebsockets.lisp` - Foreign library loading
- `grovel.lisp` - CFFI-Grovel definitions for C structs and constants
- `wrappers.lisp` - CFFI wrapper for C bit-field manipulation
- `lws.lisp` - Main implementation (MQTT client, event loop)

## Building

The project uses CFFI-Grovel to generate C struct bindings. Build artifacts are
generated automatically when loading the system.

## LWS Log Levels

To adjust libwebsockets logging, modify the log level in `qqq`:

```
LLL_ERR     = 1    - Errors
LLL_WARN    = 2    - Warnings
LLL_NOTICE  = 4    - Important notices (N:)
LLL_INFO    = 8    - Informational (I:)
LLL_DEBUG   = 16   - Debug messages (D:)
LLL_PARSER  = 32   - Parser debug
LLL_HEADER  = 64   - Header debug
LLL_EXT     = 128  - Extension debug
LLL_CLIENT  = 256  - Client debug
LLL_LATENCY = 512  - Latency debug
LLL_USER    = 1024 - User messages
```

Current default: 7 (ERR + WARN + NOTICE). For full logging, use 255 or 4095.
