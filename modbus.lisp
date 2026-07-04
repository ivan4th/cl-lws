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
