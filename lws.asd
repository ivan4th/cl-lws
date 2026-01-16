;;;; -*- Mode: Lisp; -*-

(in-package #:cl-user)

(asdf:defsystem #:lws
  :description "libwebsockets bindings for Common Lisp."
  :serial t
  :components ((:file "package")
               (:file "libwebsockets")
	       (:cffi-grovel-file "grovel")
               (:cffi-wrapper-file "wrappers")
	       (:file "lws"))
  :depends-on (:alexandria :i4-diet-utils :iterate :cffi :bordeaux-threads)
  :defsystem-depends-on (:cffi-grovel))
