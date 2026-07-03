(in-package :lws.tests)

(defparameter *test-timeout-seconds* 15)

(defun call-with-test-context (fn)
  "Run FN with a fresh lws context, then drive the event loop until
STOP-CONTEXT; error out if the test timeout is hit first."
  (with-lws-context (context)
    (let ((timed-out-p nil))
      (schedule context *test-timeout-seconds*
                #'(lambda ()
                    (setf timed-out-p t)
                    (stop-context context)))
      (funcall fn context)
      (run-context context)
      (when timed-out-p
        (error "lws test timed out")))))

;;; timers

(deftest test-lws-timers () ()
  (let ((events '()))
    (call-with-test-context
     #'(lambda (context)
         ;; one-shot ordering: 10ms before 50ms
         (schedule context 0.05 #'(lambda () (push :b events)))
         (schedule context 0.01 #'(lambda () (push :a events)))
         ;; repeating timer canceled from its own thunk on the 3rd firing
         (let ((count 0)
               (repeat-timer nil))
           (setf repeat-timer
                 (schedule context 0.02
                           #'(lambda ()
                               (when (= (incf count) 3)
                                 (push :r3 events)
                                 (cancel-timer repeat-timer)
                                 ;; canceling again must be a no-op
                                 (cancel-timer repeat-timer)))
                           :repeat t)))
         ;; a timer canceled before it fires (nil wait = next loop wave)
         (let ((canceled (schedule context 0.03 #'(lambda () (push :never events)))))
           (schedule context nil #'(lambda () (cancel-timer canceled))))
         (schedule context 0.15
                   #'(lambda ()
                       (push :end events)
                       (stop-context context)))))
    (is (equal '(:a :b :r3 :end) (reverse events)))))

;;; defer

(deftest test-lws-defer-same-thread () ()
  ;; the threadless path: defer called from a timer thunk on the loop
  ;; thread still goes through the queue + cancel_service wakeup
  (let ((ran-p nil))
    (call-with-test-context
     #'(lambda (context)
         (schedule context nil
                   #'(lambda ()
                       (defer context
                              #'(lambda ()
                                  (setf ran-p t)
                                  (stop-context context)))))))
    (is (eq t ran-p))))

#+lws-threads
(deftest test-lws-defer-cross-thread () ()
  (let ((loop-thread (bt2:current-thread))
        (ran-on nil))
    (call-with-test-context
     #'(lambda (context)
         (bt2:make-thread
          #'(lambda ()
              (defer context
                     #'(lambda ()
                         (setf ran-on (bt2:current-thread))
                         (stop-context context))))
          :name "lws defer test")))
    (is (eq loop-thread ran-on))))

;;; raw tcp loopback echo

(defclass echo-server () ((closed-count :initform 0 :accessor closed-count)))

(defmethod on-raw-accept ((handler echo-server) listener conn)
  (declare (ignore listener conn))
  handler)

(defmethod on-raw-rx ((handler echo-server) conn octets)
  (raw-write conn octets))

(defmethod on-raw-closed ((handler echo-server) conn)
  (declare (ignore conn))
  (incf (closed-count handler)))

(defclass echo-client ()
  ((context :initarg :context :reader echo-client-context)
   (payload :initarg :payload :reader echo-client-payload)
   (received :initform (make-array 0 :element-type '(unsigned-byte 8)
                                     :adjustable t :fill-pointer t)
             :reader echo-client-received)
   (connected-p :initform nil :accessor echo-client-connected-p)
   (closed-p :initform nil :accessor echo-client-closed-p)))

(defmethod on-raw-connected ((handler echo-client) conn)
  (setf (echo-client-connected-p handler) t)
  (raw-write conn (echo-client-payload handler)))

(defmethod on-raw-connect-error ((handler echo-client) conn message)
  (declare (ignore conn))
  (error "echo client connect error: ~a" message))

(defmethod on-raw-rx ((handler echo-client) conn octets)
  (let ((received (echo-client-received handler)))
    (iter (for octet in-vector octets)
          (vector-push-extend octet received))
    (when (>= (length received) (length (echo-client-payload handler)))
      (raw-close conn))))

(defmethod on-raw-closed ((handler echo-client) conn)
  (declare (ignore conn))
  (setf (echo-client-closed-p handler) t)
  (stop-context (echo-client-context handler)))

(defun run-echo-test (payload)
  (let ((server (make-instance 'echo-server))
        (client nil))
    (call-with-test-context
     #'(lambda (context)
         (let ((listener (raw-listen context 0 server :iface "127.0.0.1")))
           (is (plusp (raw-listener-port listener)))
           (setf client (make-instance 'echo-client
                                       :context context :payload payload))
           (raw-connect context "127.0.0.1" (raw-listener-port listener)
                        client))))
    (is (echo-client-connected-p client))
    (is (echo-client-closed-p client))
    (is (equalp payload (coerce (echo-client-received client)
                                '(simple-array (unsigned-byte 8) (*)))))
    (is (= 1 (closed-count server)))))

(deftest test-lws-raw-echo () ()
  (run-echo-test
   (babel:string-to-octets "hello over raw lws socket" :encoding :utf-8)))

(deftest test-lws-raw-echo-large () ()
  ;; large payload: exercises fragmented RX (pt_serv_buf_size chunks)
  ;; and multi-round writable flushing on both sides
  (let ((payload (make-array 300000 :element-type '(unsigned-byte 8))))
    (dotimes (i (length payload))
      (setf (aref payload i) (mod (* i 31) 256)))
    (run-echo-test payload)))
