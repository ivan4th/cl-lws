

#++
(ql:quickload '(alexandria i4-diet-utils iterate cffi cl-async))

(in-package :lws)

;; QQQQQ: rmme
(defun qqq-grovel ()
  (cffi-grovel:process-grovel-file "~/devlibs/cl-lws/grovel.lisp")
  (cffi-grovel:process-wrapper-file "~/devlibs/cl-lws/wrappers.lisp" :lib-soname "lws-wrappers")
  (load "~/devlibs/cl-lws/grovel__grovel.grovel-tmp.lisp")
  ;; TBD: load wrappers
  )

(defun memset (p type &optional (count 1) (val 0))
  (dotimes (i (* count (cffi:foreign-type-size type)))
    (setf (cffi:mem-aref p :uint8 i) val)))

;; TBD: support SBCL w/o threads
(defvar *lws-context-lock* (bt2:make-lock))
(defvar *lws-last-context-id* 0)
(defvar *lws-contexts* (make-hash-table))
(defvar *lws-client-lock* (bt2:make-lock))
(defvar *lws-last-client-id* 0)
(defvar *lws-clients* (make-hash-table))

(defun lws-context-from-user-ptr (ptr)
  (or (bt2:with-lock-held (*lws-context-lock*)
        (gethash (cffi:mem-ref ptr 'uint64-t) *lws-contexts*))
      (error "bad context ptr")))

(defun lws-ensure-user-data (ptr constructor)
  (bt2:with-lock-held (*lws-client-lock*)
    (let ((index (cffi:mem-ref ptr 'uint64-t)))
      (if (zerop index)
          (let ((index (incf *lws-last-client-id*)))
            (setf (cffi:mem-ref ptr 'uint64-t)
                  index
                  (gethash index *lws-clients*)
                  (funcall constructor)))
          (gethash index *lws-clients*)))))

(defun lws-free-user-data (ptr)
  (bt2:with-lock-held (*lws-client-lock*)
    (let ((index (cffi:mem-ref ptr 'uint64-t)))
      (unless (zerop index)
        (unless (remhash index *lws-clients*)
          (error "user data freed twice"))
        (setf (cffi:mem-ref ptr 'uint64-t) 0)))))

(cffi:defcfun (%lws-create-context "lws_create_context" :library libwebsockets)
    :pointer
  (info :pointer))

(cffi:defcfun (%lws-context-destroy "lws_context_destroy" :library libwebsockets)
    :void
  (context :pointer))

(cffi:defcfun (%lws-cancel-service "lws_cancel_service" :library libwebsockets)
    :void
  (context :pointer))

(cffi:defcfun (%lws-service "lws_service" :library libwebsockets)
    :int
  (context :pointer)
  (timeout-ms :int))

(cffi:defcfun (%lws-client-connect-via-info "lws_client_connect_via_info")
    :pointer
  (info :pointer))

(cffi:defcfun (%lws-callback-on-writable "lws_callback_on_writable")
    :int
  (wsi :pointer))

(cffi:defcfun (%lws-context-user "lws_context_user") :pointer (context :pointer))

(cffi:defcfun (%lws-get-context "lws_get_context") :pointer (wsi :pointer))

(cffi:defcfun (%lws-mqtt-client-send-publish "lws_mqtt_client_send_publish")
    :pointer
  (wsi :pointer)
  (pub-param :pointer)
  (buf :pointer)
  (len uint32-t)
  (final :int))

(cffi:defcfun (%lws-mqtt-client-send-subcribe "lws_mqtt_client_send_subcribe")
    :int
  (wsi :pointer)
  (sub-param :pointer))

(cffi:defcfun (%lws-mqtt-client-send-unsubcribe "lws_mqtt_client_send_unsubcribe")
    :int
  (wsi :pointer)
  (sub-param :pointer))

(cffi:defcfun (%lws-set-log-level "lws_set_log_level")
    :void
  (level :int)
  (log-emit :pointer))

(defmacro with-lws-object ((var type &rest vals) &body body)
  (let ((count 1))
    (when (and (consp type) (numberp (cdr type)))
      (setf count (cdr type) type (car type)))
    `(cffi:with-foreign-object (,var ',type ,count)
       (memset ,var ',type ,count)
       ,@(when vals
           `((setf ,@(iter (for (name value) on vals by #'cddr)
                           (collect `(cffi:foreign-slot-value ,var ',type ',name))
                           (collect value)))))
       ,(maybe-progn body))))

(defmacro with-lws-objects ((&rest defs) &body body)
  (if (null defs)
      (maybe-progn body)
      `(with-lws-object ,(first defs)
         (with-lws-objects ,(rest defs)
           ,@body))))

(defun connect-client (ctx)
  (cffi:with-foreign-strings ((client-id-str (format nil "cl-lws-mqtt-~d" (random (expt 10 20))))
                              (address-str "localhost")
                              (host-str "localhost")
                              (username-str "hass")
                              (password-str "hass")
                              (protocol-str "mqtt")
                              (method-str "MQTT")
                              (alpn-str "mqtt"))
    (with-lws-objects ((param
                        (:struct lws-mqtt-client-connect-param)
                        client-id client-id-str
                        keep-alive 60
                        username username-str
                        password password-str)
                       (info
                        (:struct lws-client-connect-info)
                        mqtt-cp param
                        address address-str
                        host host-str
                        protocol protocol-str
                        context ctx
                        method method-str
                        alpn alpn-str
                        port 1883
                        ;; TBD: SSL:
                        ;; i.ssl_connection = LCCSCF_USE_SSL;
		        ;; i.ssl_connection |= LCCSCF_ALLOW_SELFSIGNED;
		        ;; i.port = 8883;
                        ))
      (set-mqtt-client-connect-param-flags param 1) ;; clean_start=1
      (when (cffi:null-pointer-p (%lws-client-connect-via-info info))
        ;; FIXME: proper error handling!
        ;; (lws-context-stop context) -- need context struct not ctx
        (warn "connection failed")))))

(cffi:defcallback system-notify-cb :int
    ((mgr :pointer)
     (link :pointer)
     (current :int)
     (target :int))
  (declare (ignore link))
  (let ((old (cffi:foreign-enum-keyword 'lws-system-states-t current :errorp nil))
        (new (cffi:foreign-enum-keyword 'lws-system-states-t target :errorp nil)))
    (when (and (eq :lws-systate-operational old)
               (eq :lws-systate-operational new))
      (connect-client (cffi:foreign-slot-value mgr '(:struct lws-state-manager) 'parent)))
    0))

(defstruct mqtt-data
  (state :initial)
  ;; For accumulating fragmented MQTT messages
  rx-topic
  (rx-chunks nil :type list))

(cffi:defcallback mqtt-callback :int
    ((wsi :pointer)
     (reason lws-callback-reason)
     (user :pointer)
     (in :pointer)
     (len size-t))
  (let ((context (lws-context-from-user-ptr (%lws-context-user (%lws-get-context wsi)))))
    (case reason
      (:lws-callback-client-connection-error
       (dbg "client connection error: ~a" (cffi:foreign-string-to-lisp in))
       (lws-context-stop context)
       0)
      (:lws-callback-mqtt-client-closed
       (dbg "client connection closed")
       (lws-free-user-data user)
       (lws-context-stop context)
       0)
      (:lws-callback-mqtt-client-established
       (dbg "connection established")
       (cond ((plusp (%lws-callback-on-writable wsi))
              0)
             (t
              (warn "lws_callback_on_writable failed")
              -1)))
      (:lws-callback-mqtt-client-writeable
       (let ((data (lws-ensure-user-data user #'make-mqtt-data)))
         (case (mqtt-data-state data)
           (:initial
            (cffi:with-foreign-string (topic-str "#")
              (with-lws-objects ((topic
                                  ;; TBD: lws-mqtt-topic-elem
                                  (:struct topic-elem)
                                  name topic-str
                                  qos :qos0)
                                 (sub-param
                                  (:struct lws-mqtt-subscribe-param)
                                  num-topics 1
                                  topic topic))
                (cond ((zerop (%lws-mqtt-client-send-subcribe wsi sub-param))
                       (setf (mqtt-data-state data) :subscribed)
                       0)
                      (t
                       (warn "subscribe failed")
                       -1)))))
           (t 0))))
      (:lws-callback-mqtt-client-rx
       (let ((data (lws-ensure-user-data user #'make-mqtt-data)))
         (cffi:with-foreign-slots ((topic topic-len payload payload-pos payload-len)
                                   in (:struct lws-mqtt-publish-param))
           ;; len is the current chunk size, payload-len is total size
           ;; payload-pos is the offset in the complete payload
           (let ((first-chunk-p (zerop payload-pos))
                 (last-chunk-p (= (+ payload-pos len) payload-len)))
             ;; Copy the current chunk as a byte vector
             (let ((chunk (cffi:foreign-array-to-lisp payload `(:array :uint8 ,len))))
               ;; On first chunk, store topic and reset buffer
               (when first-chunk-p
                 (setf (mqtt-data-rx-topic data)
                       (cffi:foreign-string-to-lisp topic
                                                    :count topic-len
                                                    :encoding :utf-8)
                       (mqtt-data-rx-chunks data) nil))
               ;; Accumulate chunk
               (push chunk (mqtt-data-rx-chunks data))
               ;; On last chunk, concatenate and print
               (when last-chunk-p
                 (let* ((chunks (nreverse (mqtt-data-rx-chunks data)))
                        (total-len (reduce #'+ chunks :key #'length))
                        (full-payload (make-array total-len :element-type '(unsigned-byte 8)))
                        (pos 0))
                   ;; Concatenate all chunks
                   (dolist (c chunks)
                     (replace full-payload c :start1 pos)
                     (incf pos (length c)))
                   ;; Convert to string, handling encoding errors
                   (let ((payload-str
                           (handler-case
                               (babel:octets-to-string full-payload :encoding :utf-8)
                             (babel-encodings:character-decoding-error ()
                               (babel:octets-to-string full-payload :encoding :latin-1)))))
                     (dbg "MQTT RECV: ~a ~s" (mqtt-data-rx-topic data) payload-str)))
                 ;; Clear accumulated data
                 (setf (mqtt-data-rx-topic data) nil
                       (mqtt-data-rx-chunks data) nil))))
           0)))
      (t 0))))

(cffi:defcallback lws-log-emit :void ((level :int) (line :string))
  (declare (ignore level))
  (dbg "LWS LOG: ~a" (trim line)))

(defstruct (lws-context (:constructor %make-lws-context (ctx)))
  ctx
  deferred
  stop-p)

(defun lws-context-stop (context)
  (setf (lws-context-stop-p context) t))

(defun call-with-lws-context (thunk)
  (cffi:with-foreign-strings ((mqtt "mqtt")
                              (app "app"))
    (cffi:with-foreign-object (id-ptr 'uint64-t)
      (with-lws-objects ((protocols
                          ((:struct lws-protocols) . 2)
                          name mqtt
                          callback (cffi:callback mqtt-callback)
                          per-session-data-size (cffi:foreign-type-size 'uint64-t))
                         (retry
                          (:struct lws-retry-bo)
                          secs-since-valid-ping 20
                          secs-since-valid-hangup 25)
                         (notify-link
                          (:struct lws-state-notify-link)
                          notify-cb (cffi:callback system-notify-cb)
                          name app)
                         (notifiers (:pointer . 2))
                         (info
                          (:struct lws-context-creation-info)
                          port +context-port-no-listen+
                          register-notifier-list notifiers
                          ;; options +lws-server-option-do-ssl-global-init+
                          protocols protocols
                          user id-ptr
                          ;; TBD: set user to allocated context id
                          ;; Place the context in the additional hash table (locked)
                          ;; Remove it in unwind-protect cleanup part below
                          ;; fd-limit-per-thread 3
                          retry-and-idle-policy retry))
        (setf (cffi:mem-ref notifiers :pointer) notify-link)
        (let* ((ctx (%lws-create-context info))
               (context (%make-lws-context ctx))
               id)
          (bt2:with-lock-held (*lws-context-lock*)
            (setf id (incf *lws-last-context-id*)
                  (cffi:mem-ref id-ptr 'uint64-t) id
                  (gethash id *lws-contexts*) context))
          (unwind-protect
               (funcall thunk context)
            (%lws-context-destroy ctx)
            (bt2:with-lock-held (*lws-context-lock*)
              (remhash id *lws-contexts*))))))))

(defun lws-loop (context)
  (flet ((service ()
           (handler-bind ((serious-condition
                            (lambda (condition)
                              (dbg "SERIOUS-CONDITION: ~S" condition)
                              ;; TBD: defer handler
                              (lws-context-stop context)
                              (%lws-cancel-service (lws-context-ctx context)))))
             (restart-bind
                 ((exit-event-loop
                    #'(lambda ()
                        (lws-context-stop context)
                        (%lws-cancel-service (lws-context-ctx context))
                        ;; TBD: there may be no CONTINUE restart
                        (invoke-restart 'continue))
                                   :report-function
                                   #'(lambda (stream)
                                       (write-string "exit the event loop." stream))))
               (let* ((swank-package (find-package :swank))
                      (quit-restart-sym (when swank-package
                                          (find-symbol (symbol-name '#:*sldb-quit-restart*)
                                                       swank-package))))
                 (if quit-restart-sym
                     (let ((old-quit-restart (symbol-value quit-restart-sym)))
                       (setf (symbol-value quit-restart-sym)
                             'exit-event-loop
                             ;; TBD: FIXME
                             #++
                             (if *safe-sldb-quit-restart*
                                 'abort-callback
                                 'exit-event-loop))
                       (unwind-protect
                            (%lws-service (lws-context-ctx context) 0)
                         (setf (symbol-value quit-restart-sym) old-quit-restart)))
                     (%lws-service (lws-context-ctx context) 0)))))))
    (iter (dolist (func (shiftf (lws-context-deferred context) '()))
            (funcall func))
          (until (or (lws-context-stop-p context)
                     (minusp (service)))))))

;; LWS log levels (from lws-logs.h):
;;   LLL_ERR     = 1    - Errors
;;   LLL_WARN    = 2    - Warnings
;;   LLL_NOTICE  = 4    - Important notices (N:)
;;   LLL_INFO    = 8    - Informational (I:)
;;   LLL_DEBUG   = 16   - Debug messages (D:)
;;   LLL_PARSER  = 32   - Parser debug
;;   LLL_HEADER  = 64   - Header debug
;;   LLL_EXT     = 128  - Extension debug
;;   LLL_CLIENT  = 256  - Client debug
;;   LLL_LATENCY = 512  - Latency debug
;;   LLL_USER    = 1024 - User messages
;; For full logging, use 4095 (all levels) or 255 (common levels)
(defun qqq ()
  (%lws-set-log-level 7 (cffi:callback lws-log-emit)) ; ERR + WARN + NOTICE
  (call-with-lws-context #'lws-loop))

;; TBD: event loop with HANDLER-BIND on SERIOUS-CONDITION to handle nonlocal exits
;; Use lws_cancel_service to interrupt lws_service call
;; See CATCH-APP-ERRORS in cl-async util/error.lisp and event-loop.lisp

;; ASDF grovel / wrapper: https://github.com/ljosa/cl-png/blob/master/png.asd#L12-L13
;; Wrapper: https://github.com/ljosa/cl-png/blob/master/wrappers.lisp

;; Foreign libraries:
;; https://github.com/ljosa/cl-png/blob/master/libpng.lisp#L3-L13

;; also: https://github.com/ghollisjr/cl-sdl2/blob/master/wrapper.lisp

;; https://mailman.common-lisp.net/pipermail/cffi-devel/2009-January/001565.html

;; static-program-op / static-image-op
;; https://www.reddit.com/r/lisp/comments/7s8n99/comment/e1ht2lo/
;; https://github.com/marcoheisig/cl-mpi
;; sh make.sh --prefix=SOMEWHERE --fancy --with-sb-linkable-runtime --with-sb-dynamic-core

;; (sb-posix:setenv "SBCL_HOME" "/usr/local/lib/sbcl" 0)
;; (asdf:operate :static-image-op :png)

;; (setf cffi-grovel::*ld-exe-flags* (append cffi-grovel::*ld-exe-flags* '("-L" "/opt/homebrew/lib")))

;; image:
;; ~/.cache/common-lisp/sbcl-2.5.0-macosx-arm64~/devlibs/cl-png/png--all-systems.image

;; wrapper example:
;; https://github.com/cffi/cffi/blob/master/examples/wrapper-example.lisp
