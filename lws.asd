;;;; -*- Mode: Lisp; -*-

(in-package #:cl-user)

(asdf:defsystem #:lws
  :description "libwebsockets bindings for Common Lisp."
  :serial t
  :components ((:file "package")
               (:file "libwebsockets")
               (:cffi-grovel-file "grovel")
               (:cffi-wrapper-file "wrappers")
               (:file "cffi")
               (:file "registry")
               (:file "context")
               (:file "raw")
               (:file "fd")
               (:file "http")
               (:file "mqtt"))
  :depends-on (:alexandria :i4-diet-utils :iterate :cffi :bordeaux-threads :babel)
  :defsystem-depends-on (:cffi-grovel))
