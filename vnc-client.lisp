(in-package :lws)

;;; A minimal RFB client over RAW-CONNECT, for the VNC server's tests
;;; and for diagnostics: RFB 3.8, security None, Raw decoding into a
;;; frame of client-format pixel values; ZRLE rectangles are skipped
;;; by their length (the encoding is recorded), a Hextile rectangle
;;; ends decoding (the client cannot skip it without a decoder, so it
;;; records the encoding and stops parsing: close it afterwards).
;;; Everything is event-loop-thread-only; the callbacks run on the
;;; loop.

(defconstant +rfb-enc-raw+ 0)
(defconstant +rfb-enc-hextile+ 5)
(defconstant +rfb-enc-zrle+ 16)
(defconstant +rfb-enc-last-rect+ -224)

(defclass rfb-client ()
  ((connection :accessor rfb-client-connection :initform nil)
   (version :accessor rfb-client-version :initarg :version :initform 38
            :documentation "The ProtocolVersion to answer with: 33, 37 or 38.")
   (state :accessor rfb-client-state :initform :connecting
          :documentation ":CONNECTING :VERSION :SECURITY :SECURITY-RESULT
:SERVER-INIT :ESTABLISHED :UNDECODABLE :FAILED :CLOSED")
   (buffer :accessor rfb-client-buffer
           :initform (make-array 0 :element-type '(unsigned-byte 8)
                                   :adjustable t :fill-pointer 0))
   (width :accessor rfb-client-width :initform 0)
   (height :accessor rfb-client-height :initform 0)
   (name :accessor rfb-client-name :initform nil)
   (frame :accessor rfb-client-frame :initform nil
          :documentation "WIDTH x HEIGHT client-format pixel values.")
   (bytes-per-pixel :accessor rfb-client-bytes-per-pixel :initform 4)
   (big-endian-p :accessor rfb-client-big-endian-p :initform nil)
   (last-rects :accessor rfb-client-last-rects :initform '()
               :documentation "(X Y W H ENCODING) of the last update.")
   (update-count :accessor rfb-client-update-count :initform 0)
   (fail-reason :accessor rfb-client-fail-reason :initform nil)
   (on-established :accessor rfb-client-on-established :initarg :on-established
                   :initform nil)
   (on-update :accessor rfb-client-on-update :initarg :on-update :initform nil)
   (on-closed :accessor rfb-client-on-closed :initarg :on-closed :initform nil)))

(defun rfb-connect (context host port &key on-established on-update on-closed
                                          (version 38))
  "Connect to the RFB server at HOST:PORT on CONTEXT, speaking RFB
VERSION (38, 37 or 33: the security handshake differs).  ON-ESTABLISHED
\(client) runs after ServerInit, ON-UPDATE (client rects) after every
complete FramebufferUpdate, ON-CLOSED (client) when the connection
ends (a failed handshake or message included: see
RFB-CLIENT-FAIL-REASON)."
  (let ((client (make-instance 'rfb-client
                               :version version
                               :on-established on-established
                               :on-update on-update
                               :on-closed on-closed)))
    (setf (rfb-client-connection client)
          (raw-connect context host port client))
    client))

(defun rfb-close (client)
  (when (and (rfb-client-connection client)
             (not (eq (rfb-client-state client) :closed)))
    (raw-close (rfb-client-connection client)))
  (values))

(defun rfb-send-octets (client octets)
  "Send raw OCTETS (a list or vector of bytes) as they are."
  (raw-write (rfb-client-connection client)
             (coerce octets '(simple-array (unsigned-byte 8) (*))))
  (values))

(defun %rfb-u16 (value)
  (list (ldb (byte 8 8) value) (ldb (byte 8 0) value)))

(defun %rfb-u32 (value)
  (list (ldb (byte 8 24) value) (ldb (byte 8 16) value)
        (ldb (byte 8 8) value) (ldb (byte 8 0) value)))

(defun rfb-request-update (client &key (incremental t) (x 0) (y 0) width height)
  "Send a FramebufferUpdateRequest (the whole frame by default)."
  (rfb-send-octets client
                   (append (list 3 (if incremental 1 0))
                           (%rfb-u16 x) (%rfb-u16 y)
                           (%rfb-u16 (or width (rfb-client-width client)))
                           (%rfb-u16 (or height (rfb-client-height client))))))

(defun %rfb-encoding-code (encoding)
  (ecase encoding
    (:raw +rfb-enc-raw+)
    (:hextile +rfb-enc-hextile+)
    (:zrle +rfb-enc-zrle+)
    (:last-rect +rfb-enc-last-rect+)
    ((integer) encoding)))

(defun rfb-set-encodings (client encodings)
  "Send SetEncodings; ENCODINGS are :RAW / :HEXTILE / :ZRLE /
:LAST-RECT or numbers, in preference order."
  (rfb-send-octets client
                   (append (list 2 0) (%rfb-u16 (length encodings))
                           (iter (for encoding in encodings)
                                 (appending (%rfb-u32
                                             (ldb (byte 32 0)
                                                  (if (integerp encoding)
                                                      encoding
                                                      (%rfb-encoding-code
                                                       encoding)))))))))

(defun rfb-set-pixel-format (client &key (bpp 32) (depth 24) big-endian-p
                                      (red-max 255) (green-max 255) (blue-max 255)
                                      (red-shift 16) (green-shift 8) (blue-shift 0))
  "Send SetPixelFormat (true colour); the defaults are the server's
native format.  Raw pixels are decoded as BPP-wide values in the
requested byte order from then on."
  (setf (rfb-client-bytes-per-pixel client) (floor bpp 8)
        (rfb-client-big-endian-p client) big-endian-p)
  (rfb-send-octets client
                   (append (list 0 0 0 0 bpp depth (if big-endian-p 1 0) 1)
                           (%rfb-u16 red-max) (%rfb-u16 green-max)
                           (%rfb-u16 blue-max)
                           (list red-shift green-shift blue-shift 0 0 0))))

(defun rfb-send-key (client keysym &key (down t))
  "Send a KeyEvent for the X KEYSYM."
  (rfb-send-octets client (append (list 4 (if down 1 0) 0 0) (%rfb-u32 keysym))))

(defun rfb-send-pointer (client x y &key (buttons 0))
  (rfb-send-octets client (append (list 5 buttons) (%rfb-u16 x) (%rfb-u16 y))))

(defun rfb-client-pixel (client x y)
  "The client-format pixel value at (X, Y) of the last decoded frame."
  (aref (rfb-client-frame client) (+ (* y (rfb-client-width client)) x)))

;;; --- the parser ---

(defun %rfb-fail (client reason)
  (setf (rfb-client-fail-reason client) reason
        (rfb-client-state client) :failed)
  (rfb-close client))

(defun %rfb-available (client)
  (fill-pointer (rfb-client-buffer client)))

(defun %rfb-consume (client count)
  "Drop COUNT bytes from the head of the buffer."
  (let ((buffer (rfb-client-buffer client)))
    (replace buffer buffer :start2 count)
    (setf (fill-pointer buffer) (- (fill-pointer buffer) count))))

(defun %rfb-ref-u16 (buffer offset)
  (logior (ash (aref buffer offset) 8) (aref buffer (1+ offset))))

(defun %rfb-ref-u32 (buffer offset)
  (logior (ash (aref buffer offset) 24) (ash (aref buffer (+ offset 1)) 16)
          (ash (aref buffer (+ offset 2)) 8) (aref buffer (+ offset 3))))

(defun %rfb-ref-pixel (client buffer offset)
  (let ((bytes (rfb-client-bytes-per-pixel client)))
    (if (rfb-client-big-endian-p client)
        (iter (for i below bytes)
              (for value initially 0 then (logior (ash value 8)
                                                  (aref buffer (+ offset i))))
              (finally (return (logior (ash value 8) (aref buffer (+ offset i))))))
        (iter (for i below bytes)
              (sum (ash (aref buffer (+ offset i)) (* 8 i)))))))

(defun %rfb-step (client)
  "Try to make progress on the buffered bytes; T when something was
consumed (call again), NIL when more bytes are needed."
  (let* ((buffer (rfb-client-buffer client))
         (available (fill-pointer buffer)))
    (case (rfb-client-state client)
      (:version
       (when (>= available 12)
         (let ((version (map 'string #'code-char (subseq buffer 0 12))))
           (%rfb-consume client 12)
           (cond ((string= version "RFB 003.008
")
                  (rfb-send-octets client
                                   (map 'list #'char-code
                                        (ecase (rfb-client-version client)
                                          (38 "RFB 003.008
")
                                          (37 "RFB 003.007
")
                                          (33 "RFB 003.003
"))))
                  (setf (rfb-client-state client) :security)
                  t)
                 (t (%rfb-fail client (list :bad-version version)) nil)))))
      (:security
       (cond ((= (rfb-client-version client) 33)
              ;; 3.3: the server dictates one type as a U32
              (when (>= available 4)
                (let ((type (%rfb-ref-u32 buffer 0)))
                  (%rfb-consume client 4)
                  (cond ((= type 1)
                         (rfb-send-octets client '(1))   ; ClientInit: shared
                         (setf (rfb-client-state client) :server-init)
                         t)
                        (t (%rfb-fail client (list :no-security-none type))
                           nil)))))
             ((and (>= available 1) (>= available (1+ (aref buffer 0))))
              (let* ((count (aref buffer 0))
                     (types (coerce (subseq buffer 1 (1+ count)) 'list)))
                (%rfb-consume client (1+ count))
                (cond ((member 1 types)
                       (rfb-send-octets client '(1))
                       (cond ((= (rfb-client-version client) 37)
                              ;; 3.7: no SecurityResult for None
                              (rfb-send-octets client '(1))
                              (setf (rfb-client-state client) :server-init))
                             (t
                              (setf (rfb-client-state client) :security-result)))
                       t)
                      (t (%rfb-fail client (list :no-security-none types))
                         nil))))))
      (:security-result
       (when (>= available 4)
         (let ((result (%rfb-ref-u32 buffer 0)))
           (%rfb-consume client 4)
           (cond ((zerop result)
                  (rfb-send-octets client '(1))   ; ClientInit: shared
                  (setf (rfb-client-state client) :server-init)
                  t)
                 (t (%rfb-fail client (list :security-failed result)) nil)))))
      (:server-init
       (when (and (>= available 24) (>= available (+ 24 (%rfb-ref-u32 buffer 20))))
         (let ((name-length (%rfb-ref-u32 buffer 20)))
           (setf (rfb-client-width client) (%rfb-ref-u16 buffer 0)
                 (rfb-client-height client) (%rfb-ref-u16 buffer 2)
                 (rfb-client-name client)
                 (map 'string #'code-char (subseq buffer 24 (+ 24 name-length)))
                 (rfb-client-frame client)
                 (make-array (* (rfb-client-width client) (rfb-client-height client))
                             :element-type '(unsigned-byte 32) :initial-element 0)
                 (rfb-client-state client) :established)
           (%rfb-consume client (+ 24 name-length))
           (when (rfb-client-on-established client)
             (funcall (rfb-client-on-established client) client))
           t)))
      (:established
       (when (>= available 1)
         (case (aref buffer 0)
           (0 (%rfb-step-update client))
           (t (%rfb-fail client (list :unexpected-server-message (aref buffer 0)))
              nil))))
      (t nil))))

(defun %rfb-step-update (client)
  "Consume one whole FramebufferUpdate if it is completely buffered."
  (let* ((buffer (rfb-client-buffer client))
         (available (fill-pointer buffer))
         (width (rfb-client-width client))
         (frame (rfb-client-frame client))
         (bytes (rfb-client-bytes-per-pixel client)))
    (when (>= available 4)
      (let ((count (%rfb-ref-u16 buffer 2))
            (offset 4)
            (rects '())
            (undecodable nil))
        ;; first pass: is the whole message here?
        (iter (repeat count)
              (unless (>= available (+ offset 12))
                (return-from %rfb-step-update nil))
              (for x = (%rfb-ref-u16 buffer offset))
              (for y = (%rfb-ref-u16 buffer (+ offset 2)))
              (for w = (%rfb-ref-u16 buffer (+ offset 4)))
              (for h = (%rfb-ref-u16 buffer (+ offset 6)))
              (for encoding = (let ((raw (%rfb-ref-u32 buffer (+ offset 8))))
                                (if (logbitp 31 raw) (- raw (ash 1 32)) raw)))
              (incf offset 12)
              (push (list x y w h encoding) rects)
              (cond ((= encoding +rfb-enc-raw+)
                     (let ((size (* w h bytes)))
                       (unless (>= available (+ offset size))
                         (return-from %rfb-step-update nil))
                       (iter (for row below h)
                             (iter (for col below w)
                                   (setf (aref frame (+ (* (+ y row) width) x col))
                                         (%rfb-ref-pixel
                                          client buffer
                                          (+ offset (* (+ (* row w) col) bytes))))))
                       (incf offset size)))
                    ((= encoding +rfb-enc-zrle+)
                     (unless (>= available (+ offset 4))
                       (return-from %rfb-step-update nil))
                     (let ((size (%rfb-ref-u32 buffer offset)))
                       (unless (>= available (+ offset 4 size))
                         (return-from %rfb-step-update nil))
                       (incf offset (+ 4 size))))
                    ((= encoding +rfb-enc-last-rect+)
                     (finish))
                    (t
                     ;; Hextile (or anything else): no decoder here;
                     ;; record it and stop parsing this connection
                     (setf undecodable encoding)
                     (finish))))
        (setf (rfb-client-last-rects client) (nreverse rects))
        (incf (rfb-client-update-count client))
        (cond (undecodable
               (setf (rfb-client-state client) :undecodable)
               (setf (fill-pointer buffer) 0))
              (t (%rfb-consume client offset)))
        (when (rfb-client-on-update client)
          (funcall (rfb-client-on-update client) client
                   (rfb-client-last-rects client)))
        (not undecodable)))))

(defmethod on-raw-connected ((client rfb-client) connection)
  (declare (ignore connection))
  (setf (rfb-client-state client) :version))

(defmethod on-raw-connect-error ((client rfb-client) connection message)
  (declare (ignore connection))
  (setf (rfb-client-fail-reason client) (list :connect-error message)
        (rfb-client-state client) :closed)
  (when (rfb-client-on-closed client)
    (funcall (rfb-client-on-closed client) client)))

(defmethod on-raw-rx ((client rfb-client) connection octets)
  (declare (ignore connection))
  (let ((buffer (rfb-client-buffer client)))
    (iter (for octet in-sequence octets)
          (vector-push-extend octet buffer))
    (iter (while (%rfb-step client)))))

(defmethod on-raw-closed ((client rfb-client) connection)
  (declare (ignore connection))
  (unless (eq (rfb-client-state client) :failed)
    (setf (rfb-client-state client) :closed))
  (when (rfb-client-on-closed client)
    (funcall (rfb-client-on-closed client) client)))
