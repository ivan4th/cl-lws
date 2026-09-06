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
   ;; vnc server
   #:vnc-server #:vnc-server-open #:vnc-server-close
   #:vnc-server-listen-port #:vnc-server-client-count
   #:vnc-server-ptr #:vnc-server-iface #:vnc-blit-function #:vnc-server-blit
   #:vnc-keysym-to-sdl #:vnc-server-client-info #:vnc-server-drop-clients
   ;; the minimal RFB client (tests, diagnostics)
   #:rfb-client #:rfb-connect #:rfb-close #:rfb-request-update
   #:rfb-set-encodings #:rfb-set-pixel-format #:rfb-send-key
   #:rfb-send-pointer #:rfb-send-octets
   #:rfb-client-state #:rfb-client-version #:rfb-client-width #:rfb-client-height
   #:rfb-client-name #:rfb-client-frame #:rfb-client-pixel
   #:rfb-client-last-rects #:rfb-client-update-count
   #:rfb-client-bytes-per-pixel #:rfb-client-fail-reason
   #:rfb-client-on-established #:rfb-client-on-update #:rfb-client-on-closed
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
   #:modbus-add-unit
   #:modbus-subscribe
   #:modbus-unsubscribe
   #:modbus-refresh-span
   #:modbus-set-unit-enabled
   #:modbus-set-poll-seq
   #:modbus-write
   #:on-modbus-span-update
   #:on-modbus-span-state
   #:on-modbus-unit-state
   #:on-modbus-write-complete
   #:on-modbus-connection-state
   #:on-modbus-log))
