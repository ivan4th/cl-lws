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

;;; ---- slave engine ----

(cffi:defcfun ("csmb_slave_create" %csmb-slave-create) :pointer
  (cx :pointer)
  (protocols :pointer)
  (transport :pointer)
  (notify :pointer)
  (opaque :pointer))

(cffi:defcfun ("csmb_slave_destroy" %csmb-slave-destroy) :void
  (slave :pointer))

(cffi:defcfun ("csmb_slave_listen_port" %csmb-slave-listen-port) :int
  (slave :pointer))

(cffi:defcfun ("csmb_slave_register_range" %csmb-slave-register-range) :int
  (slave :pointer)
  (unit :uint8)
  (reg-type csmb-reg-type)
  (start :uint16)
  (count :uint16)
  (writable :int))

(cffi:defcfun ("csmb_slave_set_safe_range" %csmb-slave-set-safe-range) :int
  (slave :pointer)
  (unit :uint8)
  (reg-type csmb-reg-type)
  (start :uint16)
  (count :uint16))

(cffi:defcfun ("csmb_slave_set_values" %csmb-slave-set-values) :int
  (slave :pointer)
  (unit :uint8)
  (reg-type csmb-reg-type)
  (start :uint16)
  (count :uint16)
  (values :pointer))

(cffi:defcfun ("csmb_slave_blacklist" %csmb-slave-blacklist) :int
  (slave :pointer)
  (unit :uint8)
  (mode csmb-blacklist-mode))

(defun %modbus-check (result context)
  "Signal an error if the engine call RESULT is a negative CSMB_E* code."
  (when (minusp result)
    (error "~a: ~a" context
           (or (cffi:foreign-enum-keyword 'csmb-error result :errorp nil)
               result)))
  result)

;;; the slave object

(defclass modbus-slave ()
  ((id :accessor modbus-slave-id)
   (context :initarg :context :reader modbus-slave-context)
   (engine-ptr :initform nil :accessor modbus-slave-engine-ptr)
   (handler :initarg :handler :accessor modbus-slave-handler)))

;;; event delivery: one shared notify callback drains the engine's batch
;;; and fans it out to the handler generics.  %MODBUS-DRAIN-EVENTS is a
;;; generic so the master engine can plug in its own draining later.

(defgeneric on-modbus-slave-write (handler slave unit reg-type start values)
  (:documentation "A master wrote to a registered writable range: VALUES
is an (unsigned-byte 16) array of the written register values (coils: one
0/1 per coil).  REG-TYPE is a keyword (:coil / :discrete / :holding /
:input); START is the first register address.")
  (:method (handler slave unit reg-type start values)
    (declare (ignore handler slave unit reg-type start values))))

(defgeneric %modbus-drain-events (object)
  (:documentation "Drain the engine's frozen event batch for OBJECT."))

(cffi:defcallback csmb-notify :void ((opaque :pointer))
  (with-callback-guard (nil)
    (when-let ((object (lws-object-from-ptr opaque nil)))
      (%modbus-drain-events object))))

(defun %modbus-event-values (ev count)
  "Copy COUNT uint16 values out of the event payload into a fresh array."
  (let ((values (cffi:foreign-slot-value ev '(:struct csmb-event) 'values))
        (result (make-array count :element-type '(unsigned-byte 16))))
    (dotimes (i count)
      (setf (aref result i) (cffi:mem-aref values :uint16 i)))
    result))

(defmethod %modbus-drain-events ((slave modbus-slave))
  (let ((engine (modbus-slave-engine-ptr slave)))
    (cffi:with-foreign-object (out :pointer)
      (let ((count (%csmb-events-get engine out)))
        (unwind-protect
             (let ((events (cffi:mem-ref out :pointer)))
               (dotimes (i count)
                 (%modbus-slave-dispatch-event
                  slave (cffi:mem-aptr events '(:struct csmb-event) i))))
          (%csmb-events-done engine))))))

(defun %modbus-slave-dispatch-event (slave ev)
  (cffi:with-foreign-slots ((ev-type unit reg-type start count)
                            ev (:struct csmb-event))
    (case (cffi:foreign-enum-keyword 'csmb-ev-type ev-type :errorp nil)
      (:slave-write
       (on-modbus-slave-write (modbus-slave-handler slave) slave
                              unit
                              (cffi:foreign-enum-keyword 'csmb-reg-type reg-type)
                              start
                              (%modbus-event-values ev count)))
      (t nil))))

;;; transport construction + public API

(defun %call-with-tcp-transport (port iface thunk)
  "Build a CSMB_TR_TCP transport on the stack and call THUNK with its
pointer.  The C side copies HOST_OR_DEVICE, so the iface string only
needs to live across the call."
  (flet ((build (iface-ptr)
           (with-lws-object (transport
                             (:struct csmb-transport)
                             kind (cffi:foreign-enum-value 'csmb-transport-kind :tcp)
                             host-or-device iface-ptr
                             port port)
             (funcall thunk transport))))
    (if iface
        (cffi:with-foreign-string (iface-str iface)
          (build iface-str))
        (build (cffi:null-pointer)))))

(defun modbus-slave-open (context transport handler)
  "Open a Modbus slave on CONTEXT.  TRANSPORT is (:tcp-listen PORT &key
IFACE) (PORT 0 = ephemeral).  HANDLER receives ON-MODBUS-SLAVE-WRITE for
register writes.  Returns a modbus-slave."
  (let* ((slave (make-instance 'modbus-slave :context context :handler handler))
         (id (register-lws-object slave))
         (ok nil))
    (setf (modbus-slave-id slave) id)
    (unwind-protect
         (progn
           (ecase (first transport)
             (:tcp-listen
              (destructuring-bind (port &key iface) (rest transport)
                (%call-with-tcp-transport
                 port iface
                 #'(lambda (transport)
                     (let ((engine (%csmb-slave-create
                                    (lws-context-ctx context)
                                    (lws-context-protocols-ptr context)
                                    transport
                                    (cffi:callback csmb-notify)
                                    (cffi:make-pointer id))))
                       (when (cffi:null-pointer-p engine)
                         (error "modbus-slave-open: csmb_slave_create failed"))
                       (setf (modbus-slave-engine-ptr slave) engine)))))))
           (setf ok t)
           slave)
      (unless ok
        (unregister-lws-object id)))))

(defun modbus-slave-close (slave)
  "Destroy the slave engine and release its id.  In-flight connections
are closed by the engine."
  (when (modbus-slave-engine-ptr slave)
    (%csmb-slave-destroy (modbus-slave-engine-ptr slave))
    (setf (modbus-slave-engine-ptr slave) nil)
    (unregister-lws-object (modbus-slave-id slave)))
  (values))

(defun modbus-slave-listen-port (slave)
  "The actual TCP port the slave listens on (after :tcp-listen 0)."
  (%csmb-slave-listen-port (modbus-slave-engine-ptr slave)))

(defun modbus-register-range (slave unit reg-type start count &key writable safe)
  "Register a served (UNIT, REG-TYPE) range [START, START+COUNT).  With
SAFE, the range accepts writes but discards them and always reads 0
\(WRITABLE is ignored).  Otherwise WRITABLE marks it writable."
  (%modbus-check
   (if safe
       (%csmb-slave-set-safe-range (modbus-slave-engine-ptr slave)
                                   unit reg-type start count)
       (%csmb-slave-register-range (modbus-slave-engine-ptr slave)
                                   unit reg-type start count
                                   (if writable 1 0)))
   "modbus-register-range"))

(defun modbus-set-values (slave unit reg-type start values)
  "Preset the register values for a range that must fall entirely within
one registered block.  VALUES is a sequence of integers."
  (let ((count (length values)))
    (cffi:with-foreign-object (buf :uint16 (max 1 count))
      (iter (for v in-sequence values)
            (for i from 0)
            (setf (cffi:mem-aref buf :uint16 i) v))
      (%modbus-check
       (%csmb-slave-set-values (modbus-slave-engine-ptr slave)
                               unit reg-type start count buf)
       "modbus-set-values"))))

(defun modbus-blacklist (slave unit mode)
  "Set the blacklist MODE (:none / :soft / :hard) for UNIT.  Soft replies
with a gateway-target exception; hard drops the connection."
  (%modbus-check
   (%csmb-slave-blacklist (modbus-slave-engine-ptr slave) unit mode)
   "modbus-blacklist"))

;;; ---- master engine ----

(cffi:defcfun ("csmb_master_create" %csmb-master-create) :pointer
  (cx :pointer)
  (client-vhost :pointer)
  (transport :pointer)
  (notify :pointer)
  (opaque :pointer))

(cffi:defcfun ("csmb_master_destroy" %csmb-master-destroy) :void
  (master :pointer))

(cffi:defcfun ("csmb_master_add_unit" %csmb-master-add-unit) :int
  (master :pointer)
  (unit :uint8)
  (request-delay-ms :uint32)
  (stale-timeout-ms :uint32)
  (flags :uint32))

(cffi:defcfun ("csmb_master_subscribe" %csmb-master-subscribe) :int32
  (master :pointer)
  (unit :uint8)
  (reg-type csmb-reg-type)
  (start :uint16)
  (count :uint16)
  (flags :uint32))

(cffi:defcfun ("csmb_master_unsubscribe" %csmb-master-unsubscribe) :int
  (master :pointer)
  (span-id :int32))

(cffi:defcfun ("csmb_master_refresh_span" %csmb-master-refresh-span) :int
  (master :pointer)
  (span-id :int32))

(cffi:defcfun ("csmb_master_set_unit_enabled" %csmb-master-set-unit-enabled) :int
  (master :pointer)
  (unit :uint8)
  (enabled :int))

(cffi:defcfun ("csmb_master_set_poll_seq" %csmb-master-set-poll-seq) :int
  (master :pointer)
  (unit :uint8)
  (reg-type csmb-reg-type)
  (ranges :pointer)
  (n size-t))

(cffi:defcfun ("csmb_master_write" %csmb-master-write) :int64
  (master :pointer)
  (unit :uint8)
  (reqs :pointer)
  (n size-t))

(cffi:defcfun ("csmb_master_set_heartbeat" %csmb-master-set-heartbeat) :void
  (master :pointer)
  (ms :uint32))

(cffi:defcfun ("csmb_master_set_response_timeout" %csmb-master-set-response-timeout) :void
  (master :pointer)
  (ms :uint32))

;;; master event handler generics (default no-ops)

(defgeneric on-modbus-span-update (handler master span-id unit reg-type start values)
  (:documentation "A subscribed span's value changed (or was first read /
force-refreshed / SPAN-ALWAYS): VALUES is an (unsigned-byte 16) array of
the span's register values (coils: one 0/1 per coil).")
  (:method (handler master span-id unit reg-type start values)
    (declare (ignore handler master span-id unit reg-type start values))))

(defgeneric on-modbus-span-state (handler master span-id unit reg-type start count state)
  (:documentation "A span's availability changed: STATE is :online /
:stale / :offline / :uncovered.")
  (:method (handler master span-id unit reg-type start count state)
    (declare (ignore handler master span-id unit reg-type start count state))))

(defgeneric on-modbus-unit-state (handler master unit state)
  (:documentation "A unit went :online / :offline.")
  (:method (handler master unit state)
    (declare (ignore handler master unit state))))

(defgeneric on-modbus-write-done (handler master op-id unit status exception aux)
  (:documentation "A queued write op finished: STATUS is :ok / :exception /
:timeout / :verify-failed / :conn-failed / :unit-disabled.  On :exception,
EXCEPTION is the modbus exception keyword and AUX the failed request index.")
  (:method (handler master op-id unit status exception aux)
    (declare (ignore handler master op-id unit status exception aux))))

(defgeneric on-modbus-conn-state (handler master state error)
  (:documentation "The transport went :connecting / :online / :offline;
ERROR is a csmb-conn-error keyword on :offline.")
  (:method (handler master state error)
    (declare (ignore handler master state error))))

(defgeneric on-modbus-log (handler master unit kind fc exception)
  (:documentation "Rate-limited diagnostics: KIND is :exception (FC and
EXCEPTION set) or :unit-timeout.")
  (:method (handler master unit kind fc exception)
    (declare (ignore handler master unit kind fc exception))))

;;; the master object

(defclass modbus-master ()
  ((id :accessor modbus-master-id)
   (context :initarg :context :reader modbus-master-context)
   (engine-ptr :initform nil :accessor modbus-master-engine-ptr)
   (handler :initarg :handler :accessor modbus-master-handler)))

(defmethod %modbus-drain-events ((master modbus-master))
  (let ((engine (modbus-master-engine-ptr master)))
    (cffi:with-foreign-object (out :pointer)
      (let ((count (%csmb-events-get engine out)))
        (unwind-protect
             (let ((events (cffi:mem-ref out :pointer)))
               (dotimes (i count)
                 (%modbus-master-dispatch-event
                  master (cffi:mem-aptr events '(:struct csmb-event) i))))
          (%csmb-events-done engine))))))

(defun %modbus-master-dispatch-event (master ev)
  (let ((handler (modbus-master-handler master)))
    (cffi:with-foreign-slots ((ev-type unit reg-type state exception aux
                               start count span-id op-id)
                              ev (:struct csmb-event))
      (flet ((reg () (cffi:foreign-enum-keyword 'csmb-reg-type reg-type :errorp nil)))
        (case (cffi:foreign-enum-keyword 'csmb-ev-type ev-type :errorp nil)
          (:span-update
           (on-modbus-span-update handler master span-id unit (reg) start
                                  (%modbus-event-values ev count)))
          (:span-state
           (on-modbus-span-state
            handler master span-id unit (reg) start count
            (cffi:foreign-enum-keyword 'csmb-span-state state :errorp nil)))
          (:unit-state
           (on-modbus-unit-state
            handler master unit
            (cffi:foreign-enum-keyword 'csmb-span-state state :errorp nil)))
          (:write-done
           (on-modbus-write-done
            handler master op-id unit
            (cffi:foreign-enum-keyword 'csmb-wr-status state :errorp nil)
            (cffi:foreign-enum-keyword 'csmb-exception exception :errorp nil)
            aux))
          (:conn-state
           (on-modbus-conn-state
            handler master
            (cffi:foreign-enum-keyword 'csmb-conn-state state :errorp nil)
            (cffi:foreign-enum-keyword 'csmb-conn-error exception :errorp nil)))
          (:log
           (on-modbus-log
            handler master unit
            (cffi:foreign-enum-keyword 'csmb-log-kind state :errorp nil)
            aux
            (cffi:foreign-enum-keyword 'csmb-exception exception :errorp nil)))
          (t nil))))))

;;; master public API

(defun modbus-master-open (context transport handler)
  "Open a Modbus master on CONTEXT.  TRANSPORT is (:tcp HOST PORT).
HANDLER receives the ON-MODBUS-* events.  Returns a modbus-master."
  (let* ((master (make-instance 'modbus-master :context context :handler handler))
         (id (register-lws-object master))
         (ok nil))
    (setf (modbus-master-id master) id)
    (unwind-protect
         (progn
           (ecase (first transport)
             (:tcp
              (destructuring-bind (host port) (rest transport)
                (%call-with-tcp-transport
                 port host
                 #'(lambda (transport)
                     (let ((engine (%csmb-master-create
                                    (lws-context-ctx context)
                                    (lws-context-client-vhost context)
                                    transport
                                    (cffi:callback csmb-notify)
                                    (cffi:make-pointer id))))
                       (when (cffi:null-pointer-p engine)
                         (error "modbus-master-open: csmb_master_create failed"))
                       (setf (modbus-master-engine-ptr master) engine)))))))
           (setf ok t)
           master)
      (unless ok
        (unregister-lws-object id)))))

(defun modbus-master-close (master)
  "Destroy the master engine and release its id.  The outgoing connection
is closed by the engine."
  (when (modbus-master-engine-ptr master)
    (%csmb-master-destroy (modbus-master-engine-ptr master))
    (setf (modbus-master-engine-ptr master) nil)
    (unregister-lws-object (modbus-master-id master)))
  (values))

(defun modbus-master-add-unit (master unit &key (request-delay-ms 0)
                                             (stale-timeout-ms 0)
                                             no-verify-write fc1516-only)
  "Register UNIT for polling.  REQUEST-DELAY-MS paces requests;
STALE-TIMEOUT-MS (0 = 20000) bounds silence before the unit's spans go
stale.  NO-VERIFY-WRITE accepts any non-exception write response;
FC1516-ONLY never uses FC5/FC6."
  (let ((flags (logior (if no-verify-write +csmb-unit-no-verify-write+ 0)
                       (if fc1516-only +csmb-unit-fc1516-only+ 0))))
    (%modbus-check
     (%csmb-master-add-unit (modbus-master-engine-ptr master) unit
                            request-delay-ms stale-timeout-ms flags)
     "modbus-master-add-unit")))

(defun modbus-master-subscribe (master unit reg-type start count &key always)
  "Subscribe to (UNIT, REG-TYPE) range [START, START+COUNT).  With ALWAYS,
every completed read publishes (not only changes).  Returns a span id."
  (%modbus-check
   (%csmb-master-subscribe (modbus-master-engine-ptr master) unit reg-type
                           start count (if always +csmb-span-always+ 0))
   "modbus-master-subscribe"))

(defun modbus-master-unsubscribe (master span-id)
  "Drop the subscription SPAN-ID."
  (%modbus-check
   (%csmb-master-unsubscribe (modbus-master-engine-ptr master) span-id)
   "modbus-master-unsubscribe"))

(defun modbus-master-refresh-span (master span-id)
  "Force the next successful read of SPAN-ID to publish, even unchanged."
  (%modbus-check
   (%csmb-master-refresh-span (modbus-master-engine-ptr master) span-id)
   "modbus-master-refresh-span"))

(defun modbus-master-set-unit-enabled (master unit enabled)
  "Enable/disable polling of UNIT.  Disabling fails its queued writes and
stales its spans; re-enabling republishes everything."
  (%modbus-check
   (%csmb-master-set-unit-enabled (modbus-master-engine-ptr master) unit
                                  (if enabled 1 0))
   "modbus-master-set-unit-enabled"))

(defun modbus-master-set-poll-seq (master unit reg-type ranges)
  "Set the explicit poll program for (UNIT, REG-TYPE): RANGES is a list of
\(START COUNT).  An empty list clears it (revert to auto-bunching)."
  (let ((n (length ranges)))
    (cffi:with-foreign-object (arr '(:struct csmb-range) (max 1 n))
      (iter (for (start count) in ranges)
            (for i from 0)
            (let ((r (cffi:mem-aptr arr '(:struct csmb-range) i)))
              (setf (cffi:foreign-slot-value r '(:struct csmb-range) 'start) start
                    (cffi:foreign-slot-value r '(:struct csmb-range) 'count) count)))
      (%modbus-check
       (%csmb-master-set-poll-seq (modbus-master-engine-ptr master) unit
                                  reg-type arr n)
       "modbus-master-set-poll-seq"))))

(defun modbus-master-write (master unit specs)
  "Enqueue a write op: SPECS is a list of (REG-TYPE START VALUES), each a
request executed back-to-back.  REG-TYPE is :coil or :holding; VALUES is a
sequence (coils: one 0/1 per coil).  Returns an op id."
  (let ((n (length specs))
        (value-bufs '()))
    (cffi:with-foreign-object (arr '(:struct csmb-write-spec) (max 1 n))
      (unwind-protect
           (progn
             (iter (for (reg-type start vals) in specs)
                   (for i from 0)
                   (let* ((cnt (length vals))
                          (vbuf (cffi:foreign-alloc :uint16 :count (max 1 cnt)))
                          (sp (cffi:mem-aptr arr '(:struct csmb-write-spec) i)))
                     (push vbuf value-bufs)
                     (iter (for v in-sequence vals)
                           (for j from 0)
                           (setf (cffi:mem-aref vbuf :uint16 j) v))
                     (setf (cffi:foreign-slot-value sp '(:struct csmb-write-spec) 'reg-type)
                           (cffi:foreign-enum-value 'csmb-reg-type reg-type)
                           (cffi:foreign-slot-value sp '(:struct csmb-write-spec) 'start) start
                           (cffi:foreign-slot-value sp '(:struct csmb-write-spec) 'count) cnt
                           (cffi:foreign-slot-value sp '(:struct csmb-write-spec) 'values) vbuf)))
             (%modbus-check
              (%csmb-master-write (modbus-master-engine-ptr master) unit arr n)
              "modbus-master-write"))
        (mapc #'cffi:foreign-free value-bufs)))))

(defun modbus-master-set-heartbeat (master ms)
  "Set the poll heartbeat period in milliseconds (default 1000)."
  (%csmb-master-set-heartbeat (modbus-master-engine-ptr master) ms))

(defun modbus-master-set-response-timeout (master ms)
  "Set the per-request response timeout in milliseconds (default 1000)."
  (%csmb-master-set-response-timeout (modbus-master-engine-ptr master) ms))
