(in-package :common-lisp-user)

;; 32-bit armhf SBCL has no thread support; cl-lws must work without
;; threads (single-threaded event loop on the main thread).  All
;; thread-related functionality is conditionalized on this feature.
(eval-when (:compile-toplevel :load-toplevel :execute)
  #+(or sb-thread ccl)
  (pushnew :lws-threads *features*))

(defpackage :lws
  (:use :cl :alexandria :i4-diet-utils :iterate)
  (:export
   ;; context / service loop
   #:make-lws-context
   #:destroy-lws-context
   #:call-with-lws-context
   #:with-lws-context
   #:run-context
   #:stop-context
   #:defer
   #:schedule
   #:cancel-timer
   #:set-log-level
   #:*debug-on-callback-error*
   ;; raw tcp
   #:raw-connect
   #:raw-listen
   #:raw-write
   #:raw-close
   #:raw-peer
   #:raw-listener-close
   #:raw-listener-port
   #:raw-connection-state
   #:raw-connection-handler
   #:on-raw-connected
   #:on-raw-connect-error
   #:on-raw-rx
   #:on-raw-writable
   #:on-raw-closed
   #:on-raw-accept
   ;; fd watching
   #:watch-fd
   #:unwatch-fd
   ;; http server
   #:http-listen
   #:http-server-close
   #:http-server-port
   #:http-respond
   #:http-request-alive-p
   #:http-request-method
   #:http-request-path
   #:http-request-query-string
   #:http-request-headers
   #:http-request-header
   #:http-request-body
   #:*http-max-body-size*
   #:*http-response-timeout-secs*
   ;; mqtt client
   #:mqtt-connect
   #:mqtt-subscribe
   #:mqtt-unsubscribe
   #:mqtt-publish
   #:mqtt-disconnect
   #:mqtt-session-established-p
   ;; modbus slave
   #:modbus-slave-open
   #:modbus-slave-close
   #:modbus-slave-listen-port
   #:modbus-register-range
   #:modbus-set-values
   #:modbus-blacklist
   #:on-modbus-slave-write
   #:modbus-test-openpty
   ;; modbus master
   #:modbus-master-open
   #:modbus-master-close
   #:modbus-master-add-unit
   #:modbus-master-subscribe
   #:modbus-master-unsubscribe
   #:modbus-master-refresh-span
   #:modbus-master-set-unit-enabled
   #:modbus-master-set-poll-seq
   #:modbus-master-write
   #:modbus-master-set-heartbeat
   #:modbus-master-set-response-timeout
   #:on-modbus-span-update
   #:on-modbus-span-state
   #:on-modbus-unit-state
   #:on-modbus-write-done
   #:on-modbus-conn-state
   #:on-modbus-log))
