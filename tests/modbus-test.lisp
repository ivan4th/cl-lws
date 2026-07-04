(in-package :lws.tests)

;;; Modbus/TCP slave loopback: a C csmb_slave listens on an ephemeral
;;; port; a plain lws raw client sends hand-built MBAP request frames and
;;; checks the responses, while a recorder handler collects the
;;; slave-write events the engine surfaces to Lisp.

;;; ---- MBAP / PDU builders ----

(defun u16-hi (v) (ldb (byte 8 8) v))
(defun u16-lo (v) (ldb (byte 8 0) v))

(defun octets (&rest bytes)
  (coerce bytes '(simple-array (unsigned-byte 8) (*))))

(defun mbap-frame (tid unit pdu)
  "Wrap PDU (an octet vector) in a Modbus/TCP MBAP header."
  (let* ((plen (length pdu))
         (len (1+ plen))                     ;; unit + PDU
         (frame (make-array (+ 7 plen) :element-type '(unsigned-byte 8))))
    (setf (aref frame 0) (u16-hi tid)
          (aref frame 1) (u16-lo tid)
          (aref frame 2) 0
          (aref frame 3) 0
          (aref frame 4) (u16-hi len)
          (aref frame 5) (u16-lo len)
          (aref frame 6) unit)
    (replace frame pdu :start1 7)
    frame))

(defun pdu-read (fc start count)
  (octets fc (u16-hi start) (u16-lo start) (u16-hi count) (u16-lo count)))

(defun pdu-write-single-coil (addr on)
  (octets 5 (u16-hi addr) (u16-lo addr) (if on #xFF 0) 0))

(defun pdu-write-single-reg (addr value)
  (octets 6 (u16-hi addr) (u16-lo addr) (u16-hi value) (u16-lo value)))

(defun pdu-write-regs (start values)
  (let* ((count (length values))
         (pdu (make-array (+ 6 (* 2 count)) :element-type '(unsigned-byte 8))))
    (setf (aref pdu 0) 16
          (aref pdu 1) (u16-hi start)
          (aref pdu 2) (u16-lo start)
          (aref pdu 3) (u16-hi count)
          (aref pdu 4) (u16-lo count)
          (aref pdu 5) (* 2 count))
    (iter (for v in-sequence values)
          (for i from 0)
          (setf (aref pdu (+ 6 (* 2 i))) (u16-hi v)
                (aref pdu (+ 7 (* 2 i))) (u16-lo v)))
    pdu))

;;; ---- response PDU decoders ----

(defun read-words (pdu)
  "The COUNT big-endian words of an FC1-4 read response."
  (let ((nbytes (aref pdu 1)))
    (iter (for i from 0 below (floor nbytes 2))
          (collect (logior (ash (aref pdu (+ 2 (* 2 i))) 8)
                           (aref pdu (+ 3 (* 2 i))))))))

(defun read-bits (pdu nbits)
  "The NBITS bits of an FC1/2 read response, LSB-first per byte."
  (iter (for i from 0 below nbits)
        (collect (if (logbitp (mod i 8) (aref pdu (+ 2 (floor i 8)))) 1 0))))

(defun write-echo (pdu)
  "(values fc start count-or-value) of an FC5/6/15/16 echo."
  (values (aref pdu 0)
          (logior (ash (aref pdu 1) 8) (aref pdu 2))
          (logior (ash (aref pdu 3) 8) (aref pdu 4))))

(defun exception-response-p (pdu fc exc)
  (and (= (length pdu) 2)
       (= (aref pdu 0) (logior fc #x80))
       (= (aref pdu 1) exc)))

;;; ---- slave-write recorder ----

(defclass mb-slave-recorder ()
  ((events :initform '() :accessor mb-recorder-events)))

(defmethod on-modbus-slave-write ((handler mb-slave-recorder) slave unit
                                  reg-type start values)
  (declare (ignore slave))
  (push (list unit reg-type start values) (mb-recorder-events handler)))

;;; ---- request-driving raw client ----

(defstruct mb-step send nresp expect-close)

(defclass mb-test-client ()
  ((context :initarg :context :reader mb-client-context)
   (steps :initarg :steps :accessor mb-client-steps)
   (awaiting :initform 0 :accessor mb-client-awaiting)
   (expecting-close :initform nil :accessor mb-client-expecting-close)
   (rx :initform (make-array 0 :element-type '(unsigned-byte 8)
                               :adjustable t :fill-pointer t)
       :reader mb-client-rx)
   (received :initform '() :accessor mb-client-received)  ;; (unit tid pdu), reversed
   (closed :initform nil :accessor mb-client-closed)))

(defun mb-parse-frame (rx)
  "If RX starts with a complete MBAP frame, return (values unit tid pdu
total-len); else NIL."
  (let ((n (fill-pointer rx)))
    (when (>= n 7)
      (let* ((len (logior (ash (aref rx 4) 8) (aref rx 5)))
             (total (+ 6 len)))
        (when (>= n total)
          (let* ((tid (logior (ash (aref rx 0) 8) (aref rx 1)))
                 (unit (aref rx 6))
                 (pdu (make-array (1- len) :element-type '(unsigned-byte 8))))
            (replace pdu rx :start2 7 :end2 total)
            (values unit tid pdu total)))))))

(defun mb-drop (rx n)
  (replace rx rx :start2 n :end2 (fill-pointer rx))
  (decf (fill-pointer rx) n))

(defun mb-consume-frames (client)
  (let ((rx (mb-client-rx client)))
    (loop while (plusp (mb-client-awaiting client)) do
      (multiple-value-bind (unit tid pdu total) (mb-parse-frame rx)
        (unless unit (return))
        (mb-drop rx total)
        (push (list unit tid pdu) (mb-client-received client))
        (decf (mb-client-awaiting client))))))

(defun mb-send-next-step (client conn)
  (let ((step (pop (mb-client-steps client))))
    (cond ((null step)
           (stop-context (mb-client-context client)))
          (t
           (setf (mb-client-awaiting client) (mb-step-nresp step)
                 (mb-client-expecting-close client) (mb-step-expect-close step))
           (raw-write conn (mb-step-send step))))))

(defmethod on-raw-connected ((client mb-test-client) conn)
  (mb-send-next-step client conn))

(defmethod on-raw-connect-error ((client mb-test-client) conn message)
  (declare (ignore conn))
  (error "modbus test client connect error: ~a" message))

(defmethod on-raw-rx ((client mb-test-client) conn octets)
  (let ((rx (mb-client-rx client)))
    (iter (for o in-vector octets)
          (vector-push-extend o rx)))
  (mb-consume-frames client)
  (when (and (zerop (mb-client-awaiting client))
             (not (mb-client-expecting-close client)))
    (mb-send-next-step client conn)))

(defmethod on-raw-closed ((client mb-test-client) conn)
  (declare (ignore conn))
  (setf (mb-client-closed client) t)
  (stop-context (mb-client-context client)))

;;; ---- the script ----

(defparameter *mb-coil-pattern* '(1 1 0 1 0 0 1 1 0 1 1 0 0 0 1 1))

(defun mb-loopback-steps ()
  (list
   ;; reads of preset values
   (make-mb-step :send (mbap-frame #x0001 1 (pdu-read 3 10 5)) :nresp 1)
   (make-mb-step :send (mbap-frame #x0002 1 (pdu-read 3 100 5)) :nresp 1)
   (make-mb-step :send (mbap-frame #x0003 1 (pdu-read 1 0 16)) :nresp 1)
   ;; FC16 write then read-back
   (make-mb-step :send (mbap-frame #x0004 1 (pdu-write-regs 10 '(7000 7001 7002)))
                 :nresp 1)
   (make-mb-step :send (mbap-frame #x0005 1 (pdu-read 3 10 3)) :nresp 1)
   ;; single-write function codes
   (make-mb-step :send (mbap-frame #x0006 1 (pdu-write-single-coil 0 t)) :nresp 1)
   (make-mb-step :send (mbap-frame #x0007 1 (pdu-write-single-reg 15 4242)) :nresp 1)
   ;; unregistered read -> illegal address
   (make-mb-step :send (mbap-frame #x0008 1 (pdu-read 3 50 3)) :nresp 1)
   ;; safe range: reads zero, writes accepted but drop no event
   (make-mb-step :send (mbap-frame #x0009 1 (pdu-read 3 200 5)) :nresp 1)
   (make-mb-step :send (mbap-frame #x000A 1 (pdu-write-regs 200 '(11 22 33)))
                 :nresp 1)
   ;; unknown and soft-blacklisted units -> gateway target
   (make-mb-step :send (mbap-frame #x000B 9 (pdu-read 3 0 2)) :nresp 1)
   (make-mb-step :send (mbap-frame #x000C 5 (pdu-read 3 0 2)) :nresp 1)
   ;; two requests in one write -> two responses
   (make-mb-step :send (concatenate '(simple-array (unsigned-byte 8) (*))
                                    (mbap-frame #x000D 1 (pdu-read 3 10 1))
                                    (mbap-frame #x000E 1 (pdu-read 3 11 1)))
                 :nresp 2)
   ;; hard-blacklisted unit -> connection closed, no response
   (make-mb-step :send (mbap-frame #x000F 6 (pdu-read 3 0 1))
                 :nresp 0 :expect-close t)))

(deftest test-modbus-slave-loopback () ()
  (let ((recorder (make-instance 'mb-slave-recorder))
        (client nil)
        (timed-out nil))
    (with-lws-context (context)
      (let ((slave (modbus-slave-open context '(:tcp-listen 0 :iface "127.0.0.1")
                                      recorder)))
        (unwind-protect
             (progn
               (schedule context *test-timeout-seconds*
                         #'(lambda ()
                             (setf timed-out t)
                             (stop-context context)))
               (modbus-register-range slave 1 :holding 10 10 :writable t)
               (modbus-register-range slave 1 :holding 100 5)
               (modbus-register-range slave 1 :coil 0 16 :writable t)
               (modbus-register-range slave 1 :holding 200 10 :safe t)
               (modbus-set-values slave 1 :holding 10
                                  '(1000 1001 1002 1003 1004
                                    1005 1006 1007 1008 1009))
               (modbus-set-values slave 1 :holding 100 '(100 200 300 400 500))
               (modbus-set-values slave 1 :coil 0 *mb-coil-pattern*)
               (modbus-blacklist slave 5 :soft)
               (modbus-blacklist slave 6 :hard)
               (let ((port (modbus-slave-listen-port slave)))
                 (is (plusp port))
                 (setf client (make-instance 'mb-test-client
                                             :context context
                                             :steps (mb-loopback-steps)))
                 (raw-connect context "127.0.0.1" port client))
               (run-context context))
          (modbus-slave-close slave))))

    (is (null timed-out))
    (is (mb-client-closed client))

    (let ((received (coerce (reverse (mb-client-received client)) 'vector))
          (events (reverse (mb-recorder-events recorder))))
      (is (= 14 (length received)))
      (flet ((r-unit (i) (first (aref received i)))
             (r-tid (i) (second (aref received i)))
             (r-pdu (i) (third (aref received i))))
        ;; preset reads
        (is (= 1 (r-unit 0)))
        (is (= #x0001 (r-tid 0)))
        (is (equal '(1000 1001 1002 1003 1004) (read-words (r-pdu 0))))
        (is (equal '(100 200 300 400 500) (read-words (r-pdu 1))))
        (is (equal *mb-coil-pattern* (read-bits (r-pdu 2) 16)))
        ;; FC16 write echo + read-back
        (is (equal '(16 10 3) (multiple-value-list (write-echo (r-pdu 3)))))
        (is (equal '(7000 7001 7002) (read-words (r-pdu 4))))
        ;; single writes
        (is (equal '(5 0 #xFF00) (multiple-value-list (write-echo (r-pdu 5)))))
        (is (equal '(6 15 4242) (multiple-value-list (write-echo (r-pdu 6)))))
        ;; unregistered read
        (is (exception-response-p (r-pdu 7) 3 2))
        ;; safe read (zeros) + safe write echo
        (is (equal '(0 0 0 0 0) (read-words (r-pdu 8))))
        (is (equal '(16 200 3) (multiple-value-list (write-echo (r-pdu 9)))))
        ;; unknown + soft-blacklisted units
        (is (= 9 (r-unit 10)))
        (is (exception-response-p (r-pdu 10) 3 #x0B))
        (is (= 5 (r-unit 11)))
        (is (exception-response-p (r-pdu 11) 3 #x0B))
        ;; concatenated pair
        (is (= #x000D (r-tid 12)))
        (is (equal '(7000) (read-words (r-pdu 12))))
        (is (= #x000E (r-tid 13)))
        (is (equal '(7001) (read-words (r-pdu 13)))))

      ;; exactly the three writable-range writes surfaced as events
      (is (= 3 (length events)))
      (destructuring-bind (e0 e1 e2) events
        (is (equal '(1 :holding 10) (subseq e0 0 3)))
        (is (equalp #(7000 7001 7002) (fourth e0)))
        (is (equal '(1 :coil 0) (subseq e1 0 3)))
        (is (equalp #(1) (fourth e1)))
        (is (equal '(1 :holding 15) (subseq e2 0 3)))
        (is (equalp #(4242) (fourth e2)))))))

;;; Modbus/TCP master loopback: a C csmb_master polls a C csmb_slave (both
;;; in one lws context).  A recorder collects the master events, and a
;;; timer-driven phase machine walks initial publish, a write op + held
;;; read-back, change detection, ALWAYS vs change-only, refresh-span,
;;; exception -> stale, poll-seq / :uncovered, disable / enable, and a
;;; transport teardown + reconnect that republishes everything.

(defclass mb-master-recorder ()
  ((context :initarg :context :accessor rec-context)
   (master  :initform nil :accessor rec-master)
   (slave   :initform nil :accessor rec-slave)
   (port    :initform 0 :accessor rec-port)
   (h1 :accessor rec-h1) (h2 :accessor rec-h2) (c1 :accessor rec-c1)
   (he :initform nil :accessor rec-he)
   (op :initform nil :accessor rec-op)
   (events  :initform '() :accessor rec-events)        ;; reversed
   (ucounts :initform (make-hash-table) :accessor rec-ucounts)
   (bases   :initform (make-hash-table) :accessor rec-bases)
   (phases  :initform #() :accessor rec-phases)
   (phase   :initform 0 :accessor rec-phase)
   (online       :initform nil :accessor rec-online)
   (online-count :initform 0 :accessor rec-online-count)
   (offline-count :initform 0 :accessor rec-offline-count)
   (base-online  :initform 0 :accessor rec-base-online)
   (base-offline :initform 0 :accessor rec-base-offline)
   (slave-writes :initform '() :accessor rec-slave-writes)   ;; reversed
   (done    :initform nil :accessor rec-done)))

(defmethod on-modbus-slave-write ((h mb-master-recorder) slave unit reg-type
                                  start values)
  (declare (ignore slave))
  (push (list unit reg-type start values) (rec-slave-writes h)))

(defun configure-loopback-slave (slave)
  "Register the ranges + presets the master loopback expects on SLAVE."
  (modbus-register-range slave 1 :holding 10 10 :writable t)
  (modbus-register-range slave 1 :holding 100 5)
  (modbus-register-range slave 1 :coil 0 16 :writable t)
  (modbus-set-values slave 1 :holding 10
                     '(1000 1001 1002 1003 1004 1005 1006 1007 1008 1009))
  (modbus-set-values slave 1 :holding 100 '(100 200 300 400 500))
  (modbus-set-values slave 1 :coil 0 *mb-coil-pattern*))

(defun uc   (rec id) (gethash id (rec-ucounts rec) 0))
(defun base (rec id) (gethash id (rec-bases rec) 0))
(defun snap! (rec id) (setf (gethash id (rec-bases rec)) (uc rec id)))

(defun ev-any (rec type test)
  (some #'(lambda (e) (and (eq (first e) type) (funcall test e)))
        (rec-events rec)))

;;; recorded event shapes (type first):
;;;   (:span-update span-id values) (:span-state span-id state)
;;;   (:unit-state unit state) (:write-done op-id status exception req-index)
;;;   (:conn-state state error) (:log kind unit fc exception)

(defmethod on-modbus-span-update ((h mb-master-recorder) master span-id values)
  (declare (ignore master))
  (push (list :span-update span-id values) (rec-events h))
  (incf (gethash span-id (rec-ucounts h) 0)))

(defmethod on-modbus-span-state ((h mb-master-recorder) master span-id state)
  (declare (ignore master))
  (push (list :span-state span-id state) (rec-events h)))

(defmethod on-modbus-unit-state ((h mb-master-recorder) master unit state)
  (declare (ignore master))
  (push (list :unit-state unit state) (rec-events h)))

(defmethod on-modbus-write-complete ((h mb-master-recorder) master op-id status
                                     &key exception request-index)
  (declare (ignore master))
  (push (list :write-done op-id status exception request-index) (rec-events h)))

(defmethod on-modbus-connection-state ((h mb-master-recorder) master state
                                       &key error)
  (declare (ignore master))
  (push (list :conn-state state error) (rec-events h))
  (case state
    (:online  (setf (rec-online h) t) (incf (rec-online-count h)))
    (:offline (incf (rec-offline-count h)))))

(defmethod on-modbus-log ((h mb-master-recorder) master kind unit
                          &key fc exception)
  (declare (ignore master))
  (push (list :log kind unit fc exception) (rec-events h)))

(defun build-master-phases (rec)
  (let ((h1 (rec-h1 rec)) (h2 (rec-h2 rec)) (c1 (rec-c1 rec)))
    (vector
     ;; 0: initial publish of all three spans, connection online
     (list #'(lambda (r) (and (rec-online r)
                              (>= (uc r h1) 1) (>= (uc r h2) 1) (>= (uc r c1) 1)))
           #'(lambda (r)
               (setf (rec-op r)
                     (modbus-write (rec-master r) 1
                                          '((:holding 10 (7000 7001 7002)))))))
     ;; 1: write completes and the held read-back publishes the new value
     (list #'(lambda (r)
               (and (ev-any r :write-done
                            #'(lambda (e) (and (eql (second e) (rec-op r))
                                               (eq (third e) :ok))))
                    (ev-any r :span-update
                            #'(lambda (e) (and (eql (second e) h1)
                                               (equalp (third e)
                                                       #(7000 7001 7002 1003 1004)))))))
           #'(lambda (r)
               (snap! r h2)
               (modbus-set-values (rec-slave r) 1 :holding 100
                                  '(111 222 333 444 555))))
     ;; 2: change detection republishes the changed holding block
     (list #'(lambda (r) (> (uc r h2) (base r h2)))
           #'(lambda (r) (snap! r h1) (modbus-refresh-span (rec-master r) h1)))
     ;; 3: refresh-span republishes the unchanged block
     (list #'(lambda (r) (> (uc r h1) (base r h1)))
           #'(lambda (r)
               (setf (rec-he r)
                     (modbus-subscribe (rec-master r) 1 :holding 50 3))))
     ;; 4: an unregistered read draws an exception -> span stale + log
     (list #'(lambda (r)
               (and (rec-he r)
                    (ev-any r :span-state
                            #'(lambda (e) (and (eql (second e) (rec-he r))
                                               (eq (third e) :stale))))
                    (ev-any r :log #'(lambda (e) (eq (second e) :exception)))))
           #'(lambda (r)
               (modbus-set-poll-seq (rec-master r) 1 :holding '((10 . 5)))))
     ;; 5: poll-seq leaves h2 uncovered
     (list #'(lambda (r)
               (ev-any r :span-state
                       #'(lambda (e) (and (eql (second e) h2)
                                          (eq (third e) :uncovered)))))
           #'(lambda (r)
               (modbus-set-poll-seq (rec-master r) 1 :holding '())
               (modbus-set-unit-enabled (rec-master r) 1 nil)))
     ;; 6: disabling takes the unit offline
     (list #'(lambda (r)
               (ev-any r :unit-state
                       #'(lambda (e) (and (eql (second e) 1) (eq (third e) :offline)))))
           #'(lambda (r) (snap! r h1)
               (modbus-set-unit-enabled (rec-master r) 1 t)))
     ;; 7: re-enabling brings it back online and republishes
     (list #'(lambda (r)
               (and (ev-any r :unit-state
                            #'(lambda (e) (and (eql (second e) 1) (eq (third e) :online))))
                    (> (uc r h1) (base r h1))))
           #'(lambda (r)
               ;; tear the transport down: hard-blacklisting the unit makes
               ;; the slave drop the master's connection on its next request
               (snap! r h1)
               (setf (rec-base-online r) (rec-online-count r)
                     (rec-base-offline r) (rec-offline-count r))
               (modbus-blacklist (rec-slave r) 1 :hard)))
     ;; 8: the connection dropped; clear the blacklist so reconnect succeeds
     (list #'(lambda (r) (> (rec-offline-count r) (rec-base-offline r)))
           #'(lambda (r) (modbus-blacklist (rec-slave r) 1 :none)))
     ;; 9: the master reconnected to the same slave and republished
     (list #'(lambda (r)
               (and (> (rec-online-count r) (rec-base-online r))
                    (> (uc r h1) (base r h1))))
           #'(lambda (r) (declare (ignore r)))))))

(defun rec-drive (rec)
  (unless (rec-done rec)
    (let ((phases (rec-phases rec)))
      (if (>= (rec-phase rec) (length phases))
          (progn (setf (rec-done rec) t) (stop-context (rec-context rec)))
          (destructuring-bind (pred action) (aref phases (rec-phase rec))
            (when (funcall pred rec)
              (funcall action rec)
              (incf (rec-phase rec))))))))

(defun first-update-values (events span-id)
  "The VALUES of the earliest span-update for SPAN-ID (EVENTS oldest-first)."
  (iter (for e in events)
        (when (and (eq (first e) :span-update) (eql (second e) span-id))
          (return (third e)))))

(deftest test-modbus-master-loopback () ()
  (let ((timed-out nil)
        (rec nil))
    (with-lws-context (context)
      (setf rec (make-instance 'mb-master-recorder :context context))
      (let ((slave (modbus-slave-open context '(:tcp-listen 0 :iface "127.0.0.1")
                                      rec)))
        (setf (rec-slave rec) slave)
        (unwind-protect
             (progn
               (schedule context *test-timeout-seconds*
                         #'(lambda () (setf timed-out t) (stop-context context)))
               (configure-loopback-slave slave)
               (let ((port (modbus-slave-listen-port slave)))
                 (is (plusp port))
                 (setf (rec-port rec) port)
                 (let ((master (modbus-master-open context
                                                   (list :tcp "127.0.0.1" port)
                                                   rec
                                                   :heartbeat 0.03
                                                   :response-timeout 0.5)))
                   (setf (rec-master rec) master)
                   ;; short stale-timeout so the delayed exception-stale
                   ;; (phase 4) resolves quickly
                   (modbus-add-unit master 1 :stale-timeout 1.0)
                   (setf (rec-h1 rec) (modbus-subscribe master 1 :holding 10 5)
                         (rec-h2 rec) (modbus-subscribe master 1 :holding 100 5)
                         (rec-c1 rec) (modbus-subscribe master 1 :coil 0 16
                                                               :mode :always))
                   (setf (rec-phases rec) (build-master-phases rec))
                   (schedule context 0.02 #'(lambda () (rec-drive rec)) :repeat t)
                   (run-context context))))
          (when (rec-master rec) (modbus-master-close (rec-master rec)))
          (modbus-slave-close slave))))

    (is (null timed-out))
    (is (rec-online rec))
    (let ((events (reverse (rec-events rec)))
          (h1 (rec-h1 rec)) (h2 (rec-h2 rec)) (c1 (rec-c1 rec)))
      ;; initial published values
      (is (equalp #(1000 1001 1002 1003 1004) (first-update-values events h1)))
      (is (equalp #(100 200 300 400 500) (first-update-values events h2)))
      (is (equalp (coerce *mb-coil-pattern* 'vector) (first-update-values events c1)))
      ;; write op completed OK
      (is (ev-any rec :write-done
                  #'(lambda (e) (and (eql (second e) (rec-op rec))
                                     (eq (third e) :ok)))))
      ;; held read-back after the write
      (is (ev-any rec :span-update
                  #'(lambda (e) (and (eql (second e) h1)
                                     (equalp (third e) #(7000 7001 7002 1003 1004))))))
      ;; change detection surfaced the new holding-100 values
      (is (ev-any rec :span-update
                  #'(lambda (e) (and (eql (second e) h2)
                                     (equalp (third e) #(111 222 333 444 555))))))
      ;; exception -> span stale + log
      (is (ev-any rec :span-state
                  #'(lambda (e) (and (eql (second e) (rec-he rec))
                                     (eq (third e) :stale)))))
      (is (ev-any rec :log #'(lambda (e) (eq (second e) :exception))))
      ;; poll-seq left a span uncovered
      (is (ev-any rec :span-state
                  #'(lambda (e) (and (eql (second e) h2) (eq (third e) :uncovered)))))
      ;; disable/enable cycled the unit's state
      (is (ev-any rec :unit-state
                  #'(lambda (e) (and (eql (second e) 1) (eq (third e) :offline)))))
      (is (ev-any rec :unit-state
                  #'(lambda (e) (and (eql (second e) 1) (eq (third e) :online)))))
      ;; ALWAYS coil polled repeatedly; change-only holding did not
      (is (>= (uc rec c1) 3))
      (is (< (uc rec h1) (uc rec c1)))
      ;; transport teardown + reconnect: went offline then back online
      (is (>= (rec-offline-count rec) 1))
      (is (>= (rec-online-count rec) 2))
      (is (ev-any rec :conn-state
                  #'(lambda (e) (and (eq (second e) :offline)
                                     (member (third e) '(:closed :timeout :connect-failed)))))))))

;;; Modbus/RTU loopback over a pty pair: a C slave on the pty master fd
;;; (:fd), a C master on the pty slave device (:serial).  Covers initial
;;; publish, a two-request write op (slave sees both writes in order, the
;;; op completes :ok, the master reads the values back), change detection,
;;; and change-only silence.

(defun build-rtu-phases (rec)
  (let ((h1 (rec-h1 rec)) (h2 (rec-h2 rec)))
    (vector
     ;; 0: initial publish of both spans
     (list #'(lambda (r) (and (rec-online r) (>= (uc r h1) 1) (>= (uc r h2) 1)))
           #'(lambda (r)
               (snap! r h1)
               (setf (rec-op r)
                     (modbus-write (rec-master r) 1
                                   '((:holding 10 (7000 7001))
                                     (:holding 12 (8000)))))))
     ;; 1: op completes ok, slave saw both writes in order, master read back
     (list #'(lambda (r)
               (and (ev-any r :write-done
                            #'(lambda (e) (and (eql (second e) (rec-op r))
                                               (eq (third e) :ok))))
                    (ev-any r :span-update
                            #'(lambda (e)
                                (and (eql (second e) h1)
                                     (equalp (third e)
                                             #(7000 7001 8000 1003 1004)))))))
           #'(lambda (r)
               ;; write the SAME values again: the read-back is unchanged,
               ;; but force-republish (fix B) must still surface an update
               (snap! r h1)
               (setf (rec-op r)
                     (modbus-write (rec-master r) 1
                                   '((:holding 10 (7000 7001))
                                     (:holding 12 (8000)))))))
     ;; 2: the unchanged write republishes h1 anyway (force after write)
     (list #'(lambda (r)
               (and (ev-any r :write-done
                            #'(lambda (e) (and (eql (second e) (rec-op r))
                                               (eq (third e) :ok))))
                    (> (uc r h1) (base r h1))))
           #'(lambda (r)
               (snap! r h2)
               (modbus-set-values (rec-slave r) 1 :holding 100
                                  '(111 222 333 444 555))))
     ;; 3: the changed block republishes
     (list #'(lambda (r) (> (uc r h2) (base r h2)))
           #'(lambda (r) (declare (ignore r)))))))

(deftest test-modbus-rtu-loopback () ()
  (let ((timed-out nil)
        (rec nil)
        (master-fd nil))
    (multiple-value-bind (mfd slave-path) (modbus-test-openpty)
      (setf master-fd mfd)
      (with-lws-context (context)
        (setf rec (make-instance 'mb-master-recorder :context context))
        (let ((slave (modbus-slave-open context (list :fd master-fd) rec)))
          (setf (rec-slave rec) slave)
          (unwind-protect
               (progn
                 (schedule context *test-timeout-seconds*
                           #'(lambda () (setf timed-out t) (stop-context context)))
                 (modbus-register-range slave 1 :holding 10 10 :writable t)
                 (modbus-register-range slave 1 :holding 100 5)
                 (modbus-set-values slave 1 :holding 10
                                    '(1000 1001 1002 1003 1004
                                      1005 1006 1007 1008 1009))
                 (modbus-set-values slave 1 :holding 100 '(100 200 300 400 500))
                 (let ((master (modbus-master-open
                                context (list :serial slave-path :baud 115200)
                                rec :heartbeat 0.03 :response-timeout 0.3)))
                   (setf (rec-master rec) master)
                   (modbus-add-unit master 1 :stale-timeout 3.0)
                   (setf (rec-h1 rec) (modbus-subscribe master 1 :holding 10 5)
                         (rec-h2 rec) (modbus-subscribe master 1 :holding 100 5))
                   (setf (rec-phases rec) (build-rtu-phases rec))
                   (schedule context 0.02 #'(lambda () (rec-drive rec)) :repeat t)
                   (run-context context)))
            (when (rec-master rec) (modbus-master-close (rec-master rec)))
            (modbus-slave-close slave)))))

    (is (null timed-out))
    (is (rec-online rec))
    (let ((events (reverse (rec-events rec)))
          (h1 (rec-h1 rec)) (h2 (rec-h2 rec))
          (writes (reverse (rec-slave-writes rec))))
      ;; initial values
      (is (equalp #(1000 1001 1002 1003 1004) (first-update-values events h1)))
      (is (equalp #(100 200 300 400 500) (first-update-values events h2)))
      ;; two-request write op: slave saw both writes in order
      (is (>= (length writes) 2))
      (is (equal '(1 :holding 10) (subseq (first writes) 0 3)))
      (is (equalp #(7000 7001) (fourth (first writes))))
      (is (equal '(1 :holding 12) (subseq (second writes) 0 3)))
      (is (equalp #(8000) (fourth (second writes))))
      ;; op completed and the master read the written values back
      (is (ev-any rec :write-done
                  #'(lambda (e) (and (eql (second e) (rec-op rec))
                                     (eq (third e) :ok)))))
      ;; force-republish (fix B): the two writes of the same value each
      ;; produced a fresh h1 update, even though the read-back was unchanged
      (is (>= (count-if #'(lambda (e)
                            (and (eq (first e) :span-update) (eql (second e) h1)
                                 (equalp (third e) #(7000 7001 8000 1003 1004))))
                        events)
              2))
      ;; change detection surfaced the new holding-100 values
      (is (ev-any rec :span-update
                  #'(lambda (e) (and (eql (second e) h2)
                                     (equalp (third e) #(111 222 333 444 555))))))
      ;; change-only: the unchanged block did not spam updates
      (is (<= (uc rec h2) 3)))))
