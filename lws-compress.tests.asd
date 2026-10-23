;;;; -*- Mode: Lisp; -*-

(in-package #:cl-user)

(asdf:defsystem #:lws-compress.tests
  :description "Tests for the gzip/zstd HTTP response compression."
  :serial t
  :pathname "tests"
  :components ((:file "compress-test"))
  :depends-on (:lws-compress :lws.tests :vtf))
