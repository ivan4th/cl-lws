(in-package :lws)

;;; An lws-context wraps a libwebsockets context configured with
;;; explicit vhosts and no default listener.  Protocol implementations
;;; (raw / http / mqtt) register their protocol name + callback via
;;; REGISTER-LWS-PROTOCOL at load time; MAKE-LWS-CONTEXT builds the C
;;; protocols array from the registry.  The context is driven by
;;; RUN-CONTEXT, a blocking service loop on a single thread; the only
;;; thread-safe entry point from other threads is DEFER (deferred
;;; thunk queue + lws_cancel_service wakeup).

(defvar *lws-protocol-registry* '()
  "List of (name callback-pointer-thunk default-p) for the shared
protocols array.  The default protocol goes into protocols[0]: lws
dispatches plain HTTP requests to the first protocol, while raw and
MQTT connections bind by explicit protocol name.")

(defun register-lws-protocol (name callback-thunk &key default)
  (let ((entry (list name callback-thunk default)))
    (if-let ((existing (assoc name *lws-protocol-registry* :test #'equal)))
      (setf (rest existing) (rest entry))
      (setf *lws-protocol-registry*
            (append *lws-protocol-registry* (list entry))))))

(defun lws-protocol-registry-ordered ()
  (append (remove-if-not #'third *lws-protocol-registry*)
          (remove-if #'third *lws-protocol-registry*)))

(defvar *debug-on-callback-error* nil
  "When true, serious conditions in lws callbacks invoke the debugger
instead of being logged (the connection is closed in the latter case).")

(defstruct (lws-context (:constructor %make-lws-context ()))
  id
  ctx
  client-vhost
  protocols-ptr
  foreign-strings ;; name strings etc. owned by the context
  (deferred-lock (make-lws-lock "lws deferred"))
  (deferred '())
  stop-p
  destroyed-p
  (scratch-ptr nil)
  (scratch-size 0))

(defun context-of-wsi (wsi)
  (let ((ctx (%lws-get-context wsi)))
    (unless (cffi:null-pointer-p ctx)
      (lws-object-from-ptr (%lws-context-user ctx) nil))))

(defun context-foreign-string (context string)
  "Allocate a foreign copy of STRING owned by CONTEXT (freed on destroy)."
  (let ((ptr (cffi:foreign-string-alloc string)))
    (push ptr (lws-context-foreign-strings context))
    ptr))

(defun context-scratch (context size)
  "Return a foreign buffer of at least SIZE bytes owned by CONTEXT."
  (when (< (lws-context-scratch-size context) size)
    (let ((new-size (max size 65536)))
      (when (lws-context-scratch-ptr context)
        (cffi:foreign-free (lws-context-scratch-ptr context)))
      (setf (lws-context-scratch-ptr context) (cffi:foreign-alloc :uint8 :count new-size)
            (lws-context-scratch-size context) new-size)))
  (lws-context-scratch-ptr context))

;;; deferred thunks (the only cross-thread entry point)

(defun defer (context thunk)
  "Run THUNK on the event loop thread as soon as possible.  Thread-safe
on builds with thread support."
  (with-lws-lock ((lws-context-deferred-lock context))
    (push thunk (lws-context-deferred context)))
  (%lws-cancel-service (lws-context-ctx context))
  (values))

(defun drain-deferred (context)
  (let ((thunks (with-lws-lock ((lws-context-deferred-lock context))
                  (nreverse (shiftf (lws-context-deferred context) '())))))
    (dolist (thunk thunks)
      (handler-bind ((serious-condition
                       #'(lambda (condition)
                           (unless *debug-on-callback-error*
                             (warn "lws deferred thunk: ~a" condition)
                             (return-from drain-deferred (values))))))
        (funcall thunk)))))

;;; protocol callback plumbing

(defmacro with-callback-guard ((&optional (error-value -1)) &body body)
  "Run BODY, logging any serious condition and returning ERROR-VALUE.
Lisp conditions must never unwind across the lws C frames."
  (with-gensyms (guard-block)
    `(block ,guard-block
       (handler-bind ((serious-condition
                        #'(lambda (condition)
                            (unless *debug-on-callback-error*
                              (warn "lws callback error: ~a" condition)
                              (return-from ,guard-block ,error-value)))))
         ,@body))))

(defmacro define-protocol-callback (name (wsi reason user in len) &body clauses)
  "Define an lws protocol callback dispatching on the callback reason.
Handles EVENT_WAIT_CANCELLED (deferred queue drain) and guards against
Lisp conditions escaping into C.  Unhandled reasons (including ones
newer than the groveled enum) return 0."
  (with-gensyms (reason-int)
    `(cffi:defcallback ,name :int ((,wsi :pointer)
                                   (,reason-int :int)
                                   (,user :pointer)
                                   (,in :pointer)
                                   (,len size-t))
       (declare (ignorable ,wsi ,user ,in ,len))
       (with-callback-guard ()
         (let ((,reason (cffi:foreign-enum-keyword 'lws-callback-reason ,reason-int
                                                   :errorp nil)))
           (case ,reason
             (:lws-callback-event-wait-cancelled
              (when-let ((context (context-of-wsi ,wsi)))
                (drain-deferred context))
              0)
             ,@clauses
             (t 0)))))))

;;; timers (lws_sul based; event loop thread only)

(defstruct (lws-timer (:constructor %make-lws-timer (context thunk repeat-usec sul-ptr)))
  context
  thunk
  repeat-usec
  sul-ptr)

(defvar *lws-timers* (make-hash-table)
  "Maps sul pointer address -> lws-timer.  Event loop thread only.")

(defun %free-timer (timer)
  (remhash (cffi:pointer-address (lws-timer-sul-ptr timer)) *lws-timers*)
  (cffi:foreign-free (lws-timer-sul-ptr timer))
  (setf (lws-timer-sul-ptr timer) nil))

(cffi:defcallback lws-sul-cb :void ((sul :pointer))
  (with-callback-guard (nil)
    (let ((timer (gethash (cffi:pointer-address sul) *lws-timers*)))
      (cond ((null timer)
             (warn "lws-sul-cb: unknown timer"))
            (t
             (if (lws-timer-repeat-usec timer)
                 (%lws-sul-schedule (lws-context-ctx (lws-timer-context timer)) 0 sul
                                    (cffi:callback lws-sul-cb)
                                    (lws-timer-repeat-usec timer))
                 (%free-timer timer))
             (funcall (lws-timer-thunk timer)))))))

(defun schedule (context seconds thunk &key repeat)
  "Schedule THUNK to run on the event loop after SECONDS (NIL = as soon
as the loop is serviced).  With REPEAT, re-run every SECONDS.  Returns
a timer object for CANCEL-TIMER.  Event loop thread only."
  (let* ((usec (round (* (or seconds 0) 1000000)))
         (sul (cffi:foreign-alloc '(:struct lws-sorted-usec-list)))
         (timer (%make-lws-timer context thunk (when repeat usec) sul)))
    (memset sul '(:struct lws-sorted-usec-list))
    (setf (gethash (cffi:pointer-address sul) *lws-timers*) timer)
    (%lws-sul-schedule (lws-context-ctx context) 0 sul (cffi:callback lws-sul-cb) usec)
    timer))

(defun cancel-timer (timer)
  "Cancel TIMER.  Idempotent.  Event loop thread only."
  (when-let ((sul (lws-timer-sul-ptr timer)))
    (when (gethash (cffi:pointer-address sul) *lws-timers*)
      (%lws-sul-cancel sul)
      (%free-timer timer)))
  (values))

(defun %cancel-context-timers (context)
  (iter (for (nil timer) in-hashtable *lws-timers*)
        (when (eq (lws-timer-context timer) context)
          (collect timer into timers))
        (finally (dolist (timer timers)
                   (cancel-timer timer)))))

;;; context lifecycle

(defun make-lws-context ()
  (let* ((context (%make-lws-context))
         (id (register-lws-object context))
         (num-protocols (length *lws-protocol-registry*)))
    (setf (lws-context-id context) id)
    (let ((protocols (cffi:foreign-alloc '(:struct lws-protocols)
                                         :count (1+ num-protocols))))
      (memset protocols '(:struct lws-protocols) (1+ num-protocols))
      (iter (for (name callback-thunk nil) in (lws-protocol-registry-ordered))
            (for i from 0)
            (let ((entry (cffi:mem-aptr protocols '(:struct lws-protocols) i)))
              (setf (cffi:foreign-slot-value entry '(:struct lws-protocols) 'name)
                    (context-foreign-string context name)
                    (cffi:foreign-slot-value entry '(:struct lws-protocols) 'callback)
                    (funcall callback-thunk))))
      (setf (lws-context-protocols-ptr context) protocols)
      (with-lws-object (info
                        (:struct lws-context-creation-info)
                        port +context-port-no-listen+
                        options +lws-server-option-explicit-vhosts+
                        user (cffi:make-pointer id))
        (let ((ctx (%lws-create-context info)))
          (when (cffi:null-pointer-p ctx)
            (unregister-lws-object id)
            (error "lws_create_context failed"))
          (setf (lws-context-ctx context) ctx))
        ;; client-only vhost for outgoing connections
        (setf (cffi:foreign-slot-value info '(:struct lws-context-creation-info) 'protocols)
              protocols
              (cffi:foreign-slot-value info '(:struct lws-context-creation-info) 'vhost-name)
              (context-foreign-string context "client"))
        (let ((vhost (%lws-create-vhost (lws-context-ctx context) info)))
          (when (cffi:null-pointer-p vhost)
            (destroy-lws-context context)
            (error "lws_create_vhost failed for the client vhost"))
          (setf (lws-context-client-vhost context) vhost))))
    context))

(defun destroy-lws-context (context)
  (unless (lws-context-destroyed-p context)
    (setf (lws-context-destroyed-p context) t)
    (%cancel-context-timers context)
    (%lws-context-destroy (lws-context-ctx context))
    (when (lws-context-protocols-ptr context)
      (cffi:foreign-free (lws-context-protocols-ptr context)))
    (dolist (ptr (lws-context-foreign-strings context))
      (cffi:foreign-string-free ptr))
    (when (lws-context-scratch-ptr context)
      (cffi:foreign-free (lws-context-scratch-ptr context)))
    (setf (lws-context-foreign-strings context) '()
          (lws-context-protocols-ptr context) nil
          (lws-context-scratch-ptr context) nil)
    (unregister-lws-object (lws-context-id context)))
  (values))

(defun call-with-lws-context (thunk)
  (let ((context (make-lws-context)))
    (unwind-protect
         (funcall thunk context)
      (destroy-lws-context context))))

(defmacro with-lws-context ((var) &body body)
  `(call-with-lws-context #'(lambda (,var) ,@body)))

;;; the service loop

(defun stop-context (context)
  "Make RUN-CONTEXT return.  Thread-safe."
  (setf (lws-context-stop-p context) t)
  (%lws-cancel-service (lws-context-ctx context))
  (values))

(defun %service-once (context)
  (handler-bind ((serious-condition
                   #'(lambda (condition)
                       (dbg "lws service loop: SERIOUS-CONDITION: ~S" condition)
                       (stop-context context))))
    (restart-bind
        ((exit-event-loop
           #'(lambda ()
               (stop-context context)
               ;; TBD: there may be no CONTINUE restart
               (invoke-restart 'continue))
           :report-function
           #'(lambda (stream)
               (write-string "exit the lws event loop." stream))))
      (let* ((swank-package (find-package :swank))
             (quit-restart-sym (when swank-package
                                 (find-symbol (symbol-name '#:*sldb-quit-restart*)
                                              swank-package))))
        (if quit-restart-sym
            (let ((old-quit-restart (symbol-value quit-restart-sym)))
              (setf (symbol-value quit-restart-sym) 'exit-event-loop)
              (unwind-protect
                   (%lws-service (lws-context-ctx context) 0)
                (setf (symbol-value quit-restart-sym) old-quit-restart)))
            (%lws-service (lws-context-ctx context) 0))))))

(defun run-context (context &key on-iteration)
  "Blocking service loop.  Returns when STOP-CONTEXT is called or the
lws context dies.  ON-ITERATION, if given, is called after each service
wave (and after draining deferred thunks)."
  (setf (lws-context-stop-p context) nil)
  (iter (drain-deferred context)
        (when on-iteration
          (funcall on-iteration))
        (until (lws-context-stop-p context))
        (while (not (minusp (%service-once context))))))

;;; logging

(cffi:defcallback lws-log-emit :void ((level :int) (line :string))
  (declare (ignore level))
  (dbg "LWS: ~a" (trim line)))

(defun set-log-level (level)
  "Set the lws log level (a bitmask; 7 = ERR+WARN+NOTICE) routed to dbg."
  (%lws-set-log-level level (cffi:callback lws-log-emit)))
