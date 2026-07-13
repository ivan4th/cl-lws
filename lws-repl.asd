;;;; -*- Mode: Lisp; -*-

(asdf:defsystem #:lws-repl
  :description "SLIME REPL integration for the lws event loop: evaluate
REPL forms on the loop thread (the cl-async-repl pattern)."
  :author "Ivan Shvedunov"
  :version "0.1"
  :components ((:file "lws-repl"))
  :depends-on (:lws :bordeaux-threads))
