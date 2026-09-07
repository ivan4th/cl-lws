;;;; -*- Mode: Lisp; -*-

(in-package #:cl-user)

(asdf:defsystem #:lws-vnc.tests
  :description "Tests for the csvnc VNC server binding."
  :serial t
  :pathname "tests"
  :components ((:file "vnc-test"))
  :depends-on (:lws-vnc :lws.tests :vtf))
