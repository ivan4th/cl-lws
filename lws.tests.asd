;;;; -*- Mode: Lisp; -*-

(in-package #:cl-user)

(asdf:defsystem #:lws.tests
  :description "Tests for the libwebsockets bindings."
  :serial t
  :pathname "tests"
  :components ((:file "package")
               (:file "lws-test")
               (:file "modbus-test"))
  :depends-on (:lws :vtf :bordeaux-threads))
