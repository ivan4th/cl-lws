(in-package :lws)

(cffi:define-foreign-library libwebsockets
  (:darwin "libwebsockets.dylib")
  #++
  (:default "libwebsockets"))

(cffi:use-foreign-library libwebsockets)
