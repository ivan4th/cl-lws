(in-package :lws)

;; lws_mqtt_*param bitfield accessors from libcsmodbus (see
;; modbus/csmb-lws-glue.c); these used to be a cffi wrapper library,
;; which forced an absolute-path .so reload from the ASDF cache in
;; saved images.
(cffi:defcfun ("csmb_glue_set_mqtt_client_connect_param_flags"
               set-mqtt-client-connect-param-flags) :void
  (param :pointer)
  (clean-start :int))

(cffi:defcfun ("csmb_glue_set_mqtt_publish_param_flags"
               set-mqtt-publish-param-flags) :void
  (param :pointer)
  (dup :int)
  (retain :int))

(cffi:defcfun ("csmb_glue_mqtt_publish_param_retain"
               mqtt-publish-param-retain) :int
  (param :pointer))

(cffi:defcfun ("csmb_glue_mqtt_publish_param_dup"
               mqtt-publish-param-dup) :int
  (param :pointer))


;;; MQTT client sessions.  Callback-only API, no promises: the caller
;;; owns reconnect policy (lws retry is used solely for keepalive ping
;;; validity).  Operations (subscribe / unsubscribe / publish) are
;;; queued and issued one at a time from the WRITEABLE callback;
;;; ack-carrying operations complete on SUBACK / UNSUBACK / PUBACK.
;;;
;;; lws does not expose a clean MQTT DISCONNECT, so MQTT-DISCONNECT
;;; closes the connection at the TCP level; the broker will publish
;;; the will message (callers publishing a retained "offline" status
;;; before disconnecting get consistent behavior either way).

(defstruct (mqtt-op (:constructor %make-mqtt-op (kind topic payload qos retain callback)))
  kind ;; :subscribe :unsubscribe :publish
  topic
  payload
  qos
  retain
  callback
  foreign-ptrs) ;; freed on completion

(defstruct (mqtt-session (:constructor %make-mqtt-session (context)))
  id
  context
  wsi
  connect-foreign-ptrs ;; connect param block + strings, freed on close
  on-established
  on-connect-error
  on-message
  on-closed
  (op-queue '())
  in-flight
  established-p
  ;; rx reassembly state
  rx-topic
  (rx-chunks '())
  rx-retain)

(defun %mqtt-alloc-string (session string)
  (let ((ptr (cffi:foreign-string-alloc string)))
    (push ptr (mqtt-session-connect-foreign-ptrs session))
    ptr))

(defun mqtt-connect (context &key (host "localhost") (port 1883) client-id
                               username password (keep-alive 60) (clean-start t)
                               will-topic will-message (will-qos :qos1) will-retain
                               on-established on-connect-error on-message on-closed)
  "Start an MQTT client connection.  Returns an mqtt-session.
ON-ESTABLISHED (session), ON-CONNECT-ERROR (session message),
ON-MESSAGE (session topic payload-octets retain-p), ON-CLOSED (session)
are called on the event loop thread."
  (let* ((session (%make-mqtt-session context))
         (id (register-lws-object session)))
    (setf (mqtt-session-id session) id
          (mqtt-session-on-established session) on-established
          (mqtt-session-on-connect-error session) on-connect-error
          (mqtt-session-on-message session) on-message
          (mqtt-session-on-closed session) on-closed)
    (let ((param (cffi:foreign-alloc '(:struct lws-mqtt-client-connect-param)))
          (retry (cffi:foreign-alloc '(:struct lws-retry-bo)))
          (ping-secs (max 5 (floor keep-alive 3))))
      (push param (mqtt-session-connect-foreign-ptrs session))
      (push retry (mqtt-session-connect-foreign-ptrs session))
      (memset param '(:struct lws-mqtt-client-connect-param))
      (memset retry '(:struct lws-retry-bo))
      (setf (cffi:foreign-slot-value param '(:struct lws-mqtt-client-connect-param)
                                     'client-id)
            (%mqtt-alloc-string session
                                (or client-id (format nil "cl-lws-~d" id)))
            (cffi:foreign-slot-value param '(:struct lws-mqtt-client-connect-param)
                                     'keep-alive)
            keep-alive)
      (when username
        (setf (cffi:foreign-slot-value param '(:struct lws-mqtt-client-connect-param)
                                       'username)
              (%mqtt-alloc-string session username)))
      (when password
        (setf (cffi:foreign-slot-value param '(:struct lws-mqtt-client-connect-param)
                                       'password)
              (%mqtt-alloc-string session password)))
      (when will-topic
        (setf (cffi:foreign-slot-value param '(:struct lws-mqtt-client-connect-param)
                                       'will-topic)
              (%mqtt-alloc-string session will-topic)
              (cffi:foreign-slot-value param '(:struct lws-mqtt-client-connect-param)
                                       'will-message)
              (%mqtt-alloc-string session (or will-message ""))
              (cffi:foreign-slot-value param '(:struct lws-mqtt-client-connect-param)
                                       'will-qos)
              will-qos
              (cffi:foreign-slot-value param '(:struct lws-mqtt-client-connect-param)
                                       'will-retain)
              (if will-retain 1 0)))
      (set-mqtt-client-connect-param-flags param (if clean-start 1 0))
      (setf (cffi:foreign-slot-value retry '(:struct lws-retry-bo)
                                     'secs-since-valid-ping)
            ping-secs
            (cffi:foreign-slot-value retry '(:struct lws-retry-bo)
                                     'secs-since-valid-hangup)
            (+ ping-secs 10))
      (cffi:with-foreign-strings ((address-str host)
                                  (host-str host)
                                  (protocol-str "mqtt")
                                  (method-str "MQTT")
                                  (alpn-str "mqtt"))
        (with-lws-object (info
                          (:struct lws-client-connect-info)
                          context (lws-context-ctx context)
                          address address-str
                          host host-str
                          port port
                          protocol protocol-str
                          method method-str
                          alpn alpn-str
                          vhost (lws-context-client-vhost context)
                          mqtt-cp param
                          retry-and-idle-policy retry
                          opaque-user-data (cffi:make-pointer id))
          (let ((wsi (%lws-client-connect-via-info info)))
            (cond ((cffi:null-pointer-p wsi)
                   (%mqtt-cleanup-session session)
                   (schedule context nil
                             #'(lambda ()
                                 (when on-connect-error
                                   (funcall on-connect-error session
                                            "connect failed")))))
                  (t
                   (setf (mqtt-session-wsi session) wsi)))))))
    session))

(defun %mqtt-fail-op (op)
  (%mqtt-free-op op)
  (when (mqtt-op-callback op)
    (funcall (mqtt-op-callback op) nil)))

(defun %mqtt-free-op (op)
  (dolist (ptr (mqtt-op-foreign-ptrs op))
    (cffi:foreign-free ptr))
  (setf (mqtt-op-foreign-ptrs op) '()))

(defun %mqtt-cleanup-session (session)
  (when (mqtt-session-id session)
    (unregister-lws-object (mqtt-session-id session))
    (setf (mqtt-session-id session) nil))
  (setf (mqtt-session-wsi session) nil
        (mqtt-session-established-p session) nil)
  (when-let ((op (shiftf (mqtt-session-in-flight session) nil)))
    (%mqtt-fail-op op))
  (dolist (op (shiftf (mqtt-session-op-queue session) '()))
    (%mqtt-fail-op op))
  (dolist (ptr (shiftf (mqtt-session-connect-foreign-ptrs session) '()))
    (cffi:foreign-free ptr)))

(defun %mqtt-enqueue (session op)
  (setf (mqtt-session-op-queue session)
        (nconc (mqtt-session-op-queue session) (list op)))
  (when (and (mqtt-session-wsi session)
             (mqtt-session-established-p session))
    (%lws-callback-on-writable (mqtt-session-wsi session)))
  (values))

(defun mqtt-subscribe (session topic &key (qos :qos0) callback)
  "Subscribe to TOPIC.  CALLBACK, if given, is called with T on SUBACK
or NIL if the session dies first."
  (%mqtt-enqueue session (%make-mqtt-op :subscribe topic nil qos nil callback)))

(defun mqtt-unsubscribe (session topic &key callback)
  (%mqtt-enqueue session (%make-mqtt-op :unsubscribe topic nil :qos0 nil callback)))

(defun mqtt-publish (session topic payload &key (qos :qos0) retain callback)
  "Publish PAYLOAD (octets or string) to TOPIC.  CALLBACK, if given, is
called with T once the message is sent (QoS 0) or acked (QoS 1)."
  (when (stringp payload)
    (setf payload (babel:string-to-octets payload :encoding :utf-8)))
  (%mqtt-enqueue session (%make-mqtt-op :publish topic payload qos retain callback)))

(defun mqtt-disconnect (session)
  "Close the connection (TCP level; see the file header note on LWT)."
  (when-let ((wsi (mqtt-session-wsi session)))
    (%lws-set-timeout wsi :pending-timeout-http-content +lws-to-kill-async+))
  (values))

;;; op issue / completion (event loop thread)

(defun %mqtt-complete-in-flight (session ok)
  (when-let ((op (shiftf (mqtt-session-in-flight session) nil)))
    (%mqtt-free-op op)
    (when (mqtt-op-callback op)
      (funcall (mqtt-op-callback op) ok)))
  (when (and (mqtt-session-op-queue session)
             (mqtt-session-wsi session))
    (%lws-callback-on-writable (mqtt-session-wsi session))))

(defun %mqtt-send-sub-param (op wsi subscribe-p)
  ;; the param block stays allocated until the op completes: lws may
  ;; refer to it until the SUBACK/UNSUBACK arrives
  (let* ((topic-str (cffi:foreign-string-alloc (mqtt-op-topic op)))
         (topics (cffi:foreign-alloc '(:struct topic-elem)))
         (param (cffi:foreign-alloc '(:struct lws-mqtt-subscribe-param))))
    (setf (mqtt-op-foreign-ptrs op) (list topic-str topics param))
    (memset topics '(:struct topic-elem))
    (memset param '(:struct lws-mqtt-subscribe-param))
    (setf (cffi:foreign-slot-value topics '(:struct topic-elem) 'name) topic-str
          (cffi:foreign-slot-value topics '(:struct topic-elem) 'qos) (mqtt-op-qos op)
          (cffi:foreign-slot-value param '(:struct lws-mqtt-subscribe-param) 'num-topics) 1
          (cffi:foreign-slot-value param '(:struct lws-mqtt-subscribe-param) 'topic) topics)
    (zerop (if subscribe-p
               (%lws-mqtt-client-send-subcribe wsi param)
               (%lws-mqtt-client-send-unsubcribe wsi param)))))

(defun %mqtt-send-publish (op wsi)
  (let ((payload (mqtt-op-payload op)))
    (cffi:with-foreign-string (topic-str (mqtt-op-topic op))
      (cffi:with-foreign-object (buf :uint8 (max 1 (length payload)))
        (copy-octets-to-foreign payload buf)
        (with-lws-object (param
                          (:struct lws-mqtt-publish-param)
                          topic topic-str
                          topic-len (length (mqtt-op-topic op))
                          qos (mqtt-op-qos op)
                          payload-len (length payload))
          (set-mqtt-publish-param-flags param 0 (if (mqtt-op-retain op) 1 0))
          (zerop (%lws-mqtt-client-send-publish wsi param buf (length payload) 1)))))))

(defun %mqtt-issue-next (session wsi)
  (cond ((mqtt-session-in-flight session)
         0) ;; waiting for an ack
        ((null (mqtt-session-op-queue session))
         0)
        (t
         (let ((op (pop (mqtt-session-op-queue session))))
           (ecase (mqtt-op-kind op)
             ((:subscribe :unsubscribe)
              (cond ((%mqtt-send-sub-param op wsi
                                           (eq (mqtt-op-kind op) :subscribe))
                     (setf (mqtt-session-in-flight session) op)
                     0)
                    (t
                     (warn "mqtt ~(~a~) failed: ~s" (mqtt-op-kind op)
                           (mqtt-op-topic op))
                     (%mqtt-fail-op op)
                     -1)))
             (:publish
              (cond ((%mqtt-send-publish op wsi)
                     (cond ((eq (mqtt-op-qos op) :qos0)
                            ;; no ack coming; complete now
                            (when (mqtt-op-callback op)
                              (funcall (mqtt-op-callback op) t))
                            (when (mqtt-session-op-queue session)
                              (%lws-callback-on-writable wsi)))
                           (t
                            (setf (mqtt-session-in-flight session) op)))
                     0)
                    (t
                     (warn "mqtt publish failed: ~s" (mqtt-op-topic op))
                     (%mqtt-fail-op op)
                     -1))))))))

;;; rx reassembly

(defun %mqtt-handle-rx (session in len)
  (cffi:with-foreign-slots ((topic topic-len payload payload-pos payload-len)
                            in (:struct lws-mqtt-publish-param))
    ;; len is the current chunk size, payload-len the total size,
    ;; payload-pos the chunk's offset in the complete payload
    (let ((first-chunk-p (zerop payload-pos))
          (last-chunk-p (= (+ payload-pos len) payload-len)))
      (when first-chunk-p
        (setf (mqtt-session-rx-topic session)
              (cffi:foreign-string-to-lisp topic :count topic-len :encoding :utf-8)
              (mqtt-session-rx-chunks session) '()
              (mqtt-session-rx-retain session)
              (plusp (mqtt-publish-param-retain in))))
      (push (foreign-octets-to-lisp payload len) (mqtt-session-rx-chunks session))
      (when last-chunk-p
        (let* ((chunks (nreverse (shiftf (mqtt-session-rx-chunks session) '())))
               (full-payload (if (rest chunks)
                                 (let ((result (make-array payload-len
                                                           :element-type '(unsigned-byte 8))))
                                   (iter (with pos = 0)
                                         (for chunk in chunks)
                                         (replace result chunk :start1 pos)
                                         (incf pos (length chunk)))
                                   result)
                                 (first chunks)))
               (rx-topic (shiftf (mqtt-session-rx-topic session) nil)))
          (when (mqtt-session-on-message session)
            (funcall (mqtt-session-on-message session)
                     session rx-topic full-payload
                     (mqtt-session-rx-retain session))))))))

;;; the protocol callback

(define-protocol-callback mqtt-callback (wsi reason user in len)
  (:lws-callback-mqtt-client-established
   (when-let ((session (wsi-object wsi nil)))
     ;; lws may have migrated the connection to a mux child stream;
     ;; writable requests must target the wsi the callbacks come on
     (setf (mqtt-session-wsi session) wsi
           (mqtt-session-established-p session) t)
     (when (mqtt-session-on-established session)
       (funcall (mqtt-session-on-established session) session))
     (when (mqtt-session-op-queue session)
       (%lws-callback-on-writable wsi)))
   0)
  (:lws-callback-client-connection-error
   (when-let ((session (wsi-object wsi nil)))
     (let ((message (if (cffi:null-pointer-p in)
                        "connection error"
                        (cffi:foreign-string-to-lisp in)))
           (on-connect-error (mqtt-session-on-connect-error session)))
       (%mqtt-cleanup-session session)
       (when on-connect-error
         (funcall on-connect-error session message))))
   0)
  (:lws-callback-mqtt-client-closed
   (when-let ((session (wsi-object wsi nil)))
     (let ((on-closed (mqtt-session-on-closed session)))
       (%mqtt-cleanup-session session)
       (when on-closed
         (funcall on-closed session))))
   0)
  (:lws-callback-mqtt-client-writeable
   (if-let ((session (wsi-object wsi nil)))
     (%mqtt-issue-next session wsi)
     0))
  (:lws-callback-mqtt-subscribed
   (when-let ((session (wsi-object wsi nil)))
     (%mqtt-complete-in-flight session t))
   0)
  (:lws-callback-mqtt-unsubscribed
   (when-let ((session (wsi-object wsi nil)))
     (%mqtt-complete-in-flight session t))
   0)
  (:lws-callback-mqtt-ack
   (when-let ((session (wsi-object wsi nil)))
     (%mqtt-complete-in-flight session t))
   0)
  (:lws-callback-mqtt-client-rx
   (when-let ((session (wsi-object wsi nil)))
     (%mqtt-handle-rx session in len))
   0))

(register-lws-protocol "mqtt" #'(lambda () (cffi:callback mqtt-callback)))
