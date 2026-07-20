(in-package :lws)

;;; Raw TCP connections (lws "raw-skt" role).  Client connections are
;;; made with RAW-CONNECT, listeners with RAW-LISTEN (one explicit
;;; vhost per listener, accepted sockets arrive via RAW_ADOPT).  The
;;; user supplies a handler object implementing the ON-RAW-* generic
;;; functions.  All calls are event loop thread only unless noted.
;;;
;;; Write discipline: lws_write may only be called from a WRITEABLE
;;; callback, so RAW-WRITE queues octets and requests a writable
;;; callback; the queue is flushed one chunk per callback.

(defgeneric on-raw-connected (handler connection)
  (:method (handler connection)
    (declare (ignore handler connection))))

(defgeneric on-raw-connect-error (handler connection message)
  (:method (handler connection message)
    (declare (ignore handler connection message))))

(defgeneric on-raw-rx (handler connection octets))

(defgeneric on-raw-writable (handler connection)
  (:documentation "Called when the output queue has fully drained.")
  (:method (handler connection)
    (declare (ignore handler connection))))

(defgeneric on-raw-closed (handler connection)
  (:method (handler connection)
    (declare (ignore handler connection))))

(defgeneric on-raw-accept (handler listener connection)
  (:documentation "Called for a new server connection.  Must return the
handler for CONNECTION, or NIL to drop it."))

(defstruct (raw-connection (:constructor %make-raw-connection (context handler)))
  id
  context
  wsi
  handler
  (state :connecting) ;; :connecting :connected :closed :connect-failed
  (out-queue '())
  (out-offset 0)
  close-requested-p)

(defstruct (raw-listener (:constructor %make-raw-listener (context handler)))
  id
  context
  vhost
  handler
  port)

(defvar *raw-listeners* (make-hash-table)
  "Maps vhost pointer address -> raw-listener.  Event loop thread only.")

(defun raw-connect (context host port handler)
  "Start a raw TCP client connection.  Returns a raw-connection whose
state is :connecting; ON-RAW-CONNECTED / ON-RAW-CONNECT-ERROR follow."
  (let* ((conn (%make-raw-connection context handler))
         (id (register-lws-object conn)))
    (setf (raw-connection-id conn) id)
    (cffi:with-foreign-strings ((address-str host)
                                (host-str host)
                                (method-str "RAW")
                                (protocol-str "cs-raw"))
      (with-lws-object (info
                        (:struct lws-client-connect-info)
                        context (lws-context-ctx context)
                        address address-str
                        host host-str
                        port port
                        method method-str
                        local-protocol-name protocol-str
                        vhost (lws-context-client-vhost context)
                        opaque-user-data (cffi:make-pointer id))
        (let ((wsi (%lws-client-connect-via-info info)))
          (cond ((cffi:null-pointer-p wsi)
                 ;; No wsi was created, so no callbacks will fire;
                 ;; deliver the error asynchronously to keep the
                 ;; caller's view consistent.
                 (setf (raw-connection-state conn) :connect-failed)
                 (unregister-lws-object id)
                 (schedule context nil
                           #'(lambda ()
                               (on-raw-connect-error handler conn "connect failed"))))
                (t
                 (setf (raw-connection-wsi conn) wsi))))))
    conn))

(defun raw-listen (context port handler &key iface)
  "Create a raw TCP listener on PORT (0 = pick an ephemeral port).
Accepted connections are announced via ON-RAW-ACCEPT on HANDLER.
Returns a raw-listener."
  (let* ((listener (%make-raw-listener context handler))
         (id (register-lws-object listener)))
    (setf (raw-listener-id listener) id)
    ;; lws stores these string pointers in the vhost without copying,
    ;; so they must outlive it
    (let ((role-str (context-foreign-string context "raw-skt"))
          (protocol-str (context-foreign-string context "cs-raw"))
          (vhost-name-str (context-foreign-string context (format nil "raw-~d" id))))
      (with-lws-object (info
                        (:struct lws-context-creation-info)
                        port port
                        protocols (lws-context-protocols-ptr context)
                        options +lws-server-option-adopt-apply-listen-accept-config+
                        listen-accept-role role-str
                        listen-accept-protocol protocol-str
                        vhost-name vhost-name-str)
        (when iface
          (setf (cffi:foreign-slot-value info '(:struct lws-context-creation-info) 'iface)
                (context-foreign-string context iface)))
        (let ((vhost (%lws-create-vhost (lws-context-ctx context) info)))
          (when (cffi:null-pointer-p vhost)
            (unregister-lws-object id)
            (error "raw-listen: lws_create_vhost failed for port ~s" port))
          (setf (raw-listener-vhost listener) vhost
                (raw-listener-port listener) (%lws-get-vhost-listen-port vhost)
                (gethash (cffi:pointer-address vhost) *raw-listeners*) listener))))
    listener))

(defun raw-listener-close (listener)
  (when-let ((vhost (raw-listener-vhost listener)))
    (remhash (cffi:pointer-address vhost) *raw-listeners*)
    (setf (raw-listener-vhost listener) nil)
    (%lws-vhost-destroy vhost)
    (unregister-lws-object (raw-listener-id listener)))
  (values))

(defun raw-write (conn octets &key (start 0) (end (length octets)))
  "Queue OCTETS for sending.  The data is copied; the caller may reuse
the buffer.  Returns the number of octets queued."
  (let ((chunk (make-array (- end start) :element-type '(unsigned-byte 8))))
    (replace chunk octets :start2 start :end2 end)
    (setf (raw-connection-out-queue conn)
          (nconc (raw-connection-out-queue conn) (list chunk)))
    (when (and (raw-connection-wsi conn)
               (eq (raw-connection-state conn) :connected))
      (%lws-callback-on-writable (raw-connection-wsi conn)))
    (length chunk)))

(defun raw-close (conn &key abort)
  "Close CONN.  Without ABORT, queued output is flushed first."
  (cond ((eq (raw-connection-state conn) :closed))
        ((or abort
             (null (raw-connection-out-queue conn))
             (null (raw-connection-wsi conn)))
         (setf (raw-connection-close-requested-p conn) t)
         (when (raw-connection-wsi conn)
           (%lws-set-timeout (raw-connection-wsi conn)
                             :pending-timeout-http-content +lws-to-kill-async+)))
        (t
         (setf (raw-connection-close-requested-p conn) t)
         (%lws-callback-on-writable (raw-connection-wsi conn))))
  (values))

(defun raw-peer (conn)
  "Peer address as a string, or NIL."
  (when-let ((wsi (raw-connection-wsi conn)))
    (cffi:with-foreign-object (buf :uint8 64)
      (%lws-get-peer-simple wsi buf 64))))

(defun %raw-flush (conn wsi)
  "Write one queued chunk (only one lws_write per writable callback).
Returns -1 to close the connection, 0 otherwise."
  (let ((queue (raw-connection-out-queue conn)))
    (cond ((null queue)
           (cond ((raw-connection-close-requested-p conn)
                  -1)
                 (t
                  (on-raw-writable (raw-connection-handler conn) conn)
                  0)))
          (t
           (let* ((chunk (first queue))
                  (offset (raw-connection-out-offset conn))
                  (remaining (- (length chunk) offset))
                  (buf (context-scratch (raw-connection-context conn)
                                        (+ +lws-pre+ remaining)))
                  (payload (cffi:inc-pointer buf +lws-pre+)))
             (copy-octets-to-foreign chunk payload :start offset)
             (let ((written (%lws-write wsi payload remaining :lws-write-http)))
               (cond ((minusp written)
                      -1)
                     (t
                      (cond ((< written remaining)
                             (incf (raw-connection-out-offset conn) written))
                            (t
                             (pop (raw-connection-out-queue conn))
                             (setf (raw-connection-out-offset conn) 0)))
                      (cond ((raw-connection-out-queue conn)
                             (%lws-callback-on-writable wsi))
                            ((raw-connection-close-requested-p conn)
                             (return-from %raw-flush -1))
                            (t
                             (on-raw-writable (raw-connection-handler conn) conn)))
                      0))))))))

(define-protocol-callback raw-callback (wsi reason user in len)
  (:lws-callback-raw-adopt
   (let ((listener (gethash (cffi:pointer-address (%lws-get-vhost wsi))
                            *raw-listeners*)))
     (cond ((null listener)
            (warn "raw adopt on an unknown vhost")
            -1)
           (t
            (let* ((context (raw-listener-context listener))
                   (conn (%make-raw-connection context nil)))
              (setf (raw-connection-id conn) (register-lws-object conn)
                    (raw-connection-wsi conn) wsi
                    (raw-connection-state conn) :connected)
              (set-wsi-object-id wsi (raw-connection-id conn))
              (let ((handler (on-raw-accept (raw-listener-handler listener)
                                            listener conn)))
                (cond ((null handler)
                       (unregister-lws-object (raw-connection-id conn))
                       -1)
                      (t
                       (setf (raw-connection-handler conn) handler)
                       0))))))))
  (:lws-callback-raw-connected
   (when-let ((conn (wsi-object wsi nil)))
     (setf (raw-connection-state conn) :connected)
     (on-raw-connected (raw-connection-handler conn) conn))
   0)
  (:lws-callback-client-connection-error
   (when-let ((conn (wsi-object wsi nil)))
     (setf (raw-connection-state conn) :connect-failed
           (raw-connection-wsi conn) nil)
     (unregister-lws-object (raw-connection-id conn))
     (on-raw-connect-error (raw-connection-handler conn) conn
                           (if (cffi:null-pointer-p in)
                               "connection error"
                               (cffi:foreign-string-to-lisp in))))
   0)
  (:lws-callback-raw-rx
   (when-let ((conn (wsi-object wsi nil)))
     (on-raw-rx (raw-connection-handler conn) conn
                (foreign-octets-to-lisp in len)))
   0)
  (:lws-callback-raw-writeable
   (if-let ((conn (wsi-object wsi nil)))
     (%raw-flush conn wsi)
     0))
  (:lws-callback-raw-close
   (when-let ((conn (wsi-object wsi nil)))
     (setf (raw-connection-state conn) :closed
           (raw-connection-wsi conn) nil)
     (unregister-lws-object (raw-connection-id conn))
     (on-raw-closed (raw-connection-handler conn) conn))
   0))

(register-lws-protocol "cs-raw" #'(lambda () (cffi:callback raw-callback)))

(defun %raw-wsi-destroy (wsi)
  ;; Backstop for teardown paths that deliver neither
  ;; CLIENT_CONNECTION_ERROR nor RAW_CLOSE: if the connection still
  ;; references the wsi being destroyed, invalidate it so nothing
  ;; pokes lws through a freed pointer, and notify the handler.
  (let ((conn (wsi-object wsi nil)))
    (when (and (raw-connection-p conn)
               (raw-connection-wsi conn)
               (cffi:pointer-eq (raw-connection-wsi conn) wsi))
      (let ((connected-p (eq (raw-connection-state conn) :connected)))
        (setf (raw-connection-state conn) (if connected-p :closed :connect-failed)
              (raw-connection-wsi conn) nil)
        (unregister-lws-object (raw-connection-id conn))
        (cond (connected-p
               (on-raw-closed (raw-connection-handler conn) conn))
              (t
               (on-raw-connect-error (raw-connection-handler conn) conn
                                     "connection destroyed")))))))

(register-wsi-destroy-handler 'raw #'%raw-wsi-destroy)
