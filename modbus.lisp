(in-package :lws)

;;; Bindings for the csmb Modbus engine (modbus/libcsmodbus).
;;;
;;; The engine batches events; Lisp gets a single notify callback per
;;; C-side activation and drains the whole batch (see modbus/csmb.h).
;;; Everything here is event-loop-thread-only.

;;; low-level FFI

(cffi:defcfun ("csmb_events_get" %csmb-events-get) size-t
  (engine :pointer)
  (out :pointer))

(cffi:defcfun ("csmb_events_done" %csmb-events-done) :void
  (engine :pointer))

(cffi:defcfun ("csmb_test_openpty" %csmb-test-openpty) :int
  (master-fd :pointer)
  (slave-path :pointer)
  (path-len size-t))

(defun modbus-test-openpty ()
  "Allocate a pty pair for RTU loopback tests.  Returns the master fd
and the slave device path."
  (cffi:with-foreign-objects ((master-fd :int)
                              (slave-path :char 256))
    (unless (zerop (%csmb-test-openpty master-fd slave-path 256))
      (error "csmb_test_openpty failed"))
    (values (cffi:mem-ref master-fd :int)
            (cffi:foreign-string-to-lisp slave-path))))

;;; the shared protocol; engines bind connections to it by name

(register-lws-protocol "cs-modbus"
                       #'(lambda ()
                           (or (cffi:foreign-symbol-pointer "csmb_lws_protocol_callback")
                               (error "libcsmodbus is not loaded"))))
