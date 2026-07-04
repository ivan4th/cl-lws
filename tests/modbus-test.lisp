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
