(in-package :lws.tests)

;;; content coding negotiation

(deftest test-http-parse-accept-encoding () ()
  (is (equal '(("gzip" . 1000) ("deflate" . 1000) ("br" . 1000) ("zstd" . 1000))
             (parse-accept-encoding "gzip, deflate, br, zstd")))
  (is (equal '(("zstd" . 0) ("gzip" . 500) ("*" . 125))
             (parse-accept-encoding "ZSTD;q=0, gzip; Q=0.5 ,*;q=0.125")))
  ;; malformed qvalues and empty codings drop the element
  (is (equal '(("gzip" . 1000))
             (parse-accept-encoding "br;q=2, gzip, zstd;q=abc, ;q=1, deflate;q=0.1234")))
  (is (equal '() (parse-accept-encoding ""))))

(deftest test-http-choose-content-coding () ()
  (let ((codings (list (cons "zstd" :zstd) (cons "gzip" :gzip))))
    (flet ((choose (header)
             (car (http-choose-content-coding header codings))))
      (is (equal "zstd" (choose "gzip, deflate, br, zstd")))
      (is (equal "gzip" (choose "gzip, deflate")))
      (is (equal "gzip" (choose "zstd;q=0, gzip")))
      (is (equal "gzip" (choose "zstd;q=0.4, gzip;q=0.5")))
      ;; a tie goes to the server's preference
      (is (equal "zstd" (choose "gzip;q=0.5, zstd;q=0.5")))
      (is (equal "zstd" (choose "*")))
      (is (equal "gzip" (choose "zstd;q=0, *")))
      (is (equal "gzip" (choose "x-gzip")))
      (is (null (choose "identity")))
      (is (null (choose "*;q=0")))
      (is (null (choose "br, deflate")))
      (is (null (choose "")))
      ;; no Accept-Encoding at all
      (is (null (choose nil))))))

(deftest test-http-compressible-content-type () ()
  (dolist (type '("text/html" "text/plain; charset=utf-8" "application/json"
                  "Application/JSON; charset=utf-8" "application/javascript"
                  "image/svg+xml" "application/ld+json"))
    (is (http-compressible-content-type-p type)))
  (dolist (type '("image/png" "application/octet-stream" "application/zstd" ""))
    (is (not (http-compressible-content-type-p type)))))

;;; a blocking HTTP/1.1 client on a thread of its own, against a server
;;; on the test context's event loop

#+(and sbcl lws-threads)
(progn
  (defun call-with-http-server (handler client)
    "Serve HANDLER, called with the context and the request, on an
ephemeral port, and run CLIENT with the port on a thread of its own
while the event loop runs here.  Returns the values CLIENT returns; an
error in CLIENT is signalled here."
    (let ((results '())
          (client-error nil)
          (thread nil))
      (call-with-test-context
       #'(lambda (context)
           (let ((server (http-listen context 0
                                      #'(lambda (request)
                                          (funcall handler context request))
                                      :iface "127.0.0.1")))
             (setf thread
                   (bt:make-thread
                    #'(lambda ()
                        (handler-case
                            (setf results (multiple-value-list
                                           (funcall client (http-server-port server))))
                          (error (condition)
                            (setf client-error condition)))
                        (stop-context context))
                    :name "lws http test client")))))
      (bt:join-thread thread)
      (when client-error
        (error client-error))
      (values-list results)))

  (defun http-test-connect (port &key receive-buffer)
    "A binary stream to the server on PORT.  RECEIVE-BUFFER sets the
socket's receive buffer before connecting, so that the window the
server sees stays that small and what the client has read is close to
what the server could send."
    (let ((socket (make-instance 'sb-bsd-sockets:inet-socket
                                 :type :stream :protocol :tcp)))
      (when receive-buffer
        (setf (sb-bsd-sockets:sockopt-receive-buffer socket) receive-buffer))
      (sb-bsd-sockets:socket-connect socket #(127 0 0 1) port)
      (sb-bsd-sockets:socket-make-stream socket :input t :output t
                                                :element-type '(unsigned-byte 8)
                                                :buffering :full)))

  (defun http-test-send-request (stream path &key accept-encoding)
    (write-sequence
     (babel:string-to-octets
      (format nil "GET ~a HTTP/1.1~c~cHost: localhost~c~c~@[Accept-Encoding: ~a~c~c~]~c~c"
              path #\Return #\Linefeed #\Return #\Linefeed
              accept-encoding #\Return #\Linefeed #\Return #\Linefeed)
      :encoding :latin-1)
     stream)
    (force-output stream))

  (defun http-test-read-response (stream &key piece (pause 0))
    "Read one response from STREAM.  Returns the status, the headers as
an alist with lowercase names, the body octets read and whether the
body arrived complete.  With PIECE the body is read PIECE octets at a
time, pausing PAUSE seconds after each piece: a slow reader."
    (let ((head (make-array 0 :element-type '(unsigned-byte 8)
                              :adjustable t :fill-pointer 0)))
      ;; the head ends with an empty line
      (iter (for octet = (read-byte stream nil))
            (unless octet
              (return-from http-test-read-response (values nil '() #() nil)))
            (vector-push-extend octet head)
            (until (and (>= (fill-pointer head) 4)
                        (equalp #(13 10 13 10) (subseq head (- (fill-pointer head) 4))))))
      (let* ((lines (iter (for line in (split-sequence:split-sequence
                                        #\Linefeed (babel:octets-to-string head :encoding :latin-1)))
                          (for trimmed = (string-right-trim '(#\Return) line))
                          (unless (emptyp trimmed)
                            (collect trimmed))))
             (status (parse-integer (second (split-sequence:split-sequence #\Space (first lines)))))
             (headers (iter (for line in (rest lines))
                            (for colon = (position #\: line))
                            (collect (cons (string-downcase (subseq line 0 colon))
                                           (string-trim " " (subseq line (1+ colon)))))))
             (length (parse-integer (or (assoc-value headers "content-length" :test #'equal) "0")))
             (body (make-array length :element-type '(unsigned-byte 8)))
             (got (handler-case
                      (iter (with got = 0)
                            (while (< got length))
                            (for n = (read-sequence body stream
                                                    :start got
                                                    :end (if piece
                                                             (min length (+ got piece))
                                                             length)))
                            (until (= n got))
                            (setf got n)
                            (when piece
                              (sleep pause))
                            (finally (return got)))
                    ;; the server may reset a connection it gave up on
                    (stream-error ()
                      nil))))
        (values status headers (subseq body 0 (or got 0)) (eql got length)))))

  (defun test-pattern (size)
    (let ((octets (make-array size :element-type '(unsigned-byte 8))))
      (dotimes (i size octets)
        (setf (aref octets i) (mod (* i 7) 251)))))

  (defun http-test-handler (&key defer-secs)
    "Answer /N with N pattern octets, as application/octet-stream so that
no content coding applies; DEFER-SECS answers from a timer."
    #'(lambda (context request)
        (let ((body (test-pattern (parse-integer (http-request-path request) :start 1))))
          (flet ((respond ()
                   (http-respond request :content-type "application/octet-stream"
                                         :body body)))
            (if defer-secs
                (schedule context defer-secs #'respond)
                (respond))))))

  (deftest test-http-body-chunking () ()
    ;; bodies on and around the chunk boundaries arrive intact, one after
    ;; another over a single keep-alive connection
    (let* ((chunk *http-write-chunk-size*)
           (sizes (list 0 1 (1- chunk) chunk (1+ chunk) (* 3 chunk) 1000000))
           (results
             (call-with-http-server
              (http-test-handler)
              #'(lambda (port)
                  (with-open-stream (stream (http-test-connect port))
                    (iter (for size in sizes)
                          (http-test-send-request stream (format nil "/~d" size))
                          (collect (multiple-value-list
                                    (http-test-read-response stream)))))))))
      (iter (for size in sizes)
            (for (status headers body complete-p) in results)
            (is (eql 200 status))
            (is (equal (princ-to-string size)
                       (assoc-value headers "content-length" :test #'equal)))
            (is complete-p)
            (is (equalp (test-pattern size) body)))))

  (deftest test-http-client-gone-mid-body () ()
    ;; a client closing its connection in the middle of a body leaves the
    ;; server serving the next one normally
    (multiple-value-bind (status complete-p body)
        (call-with-http-server
         (http-test-handler)
         #'(lambda (port)
             (with-open-stream (stream (http-test-connect port :receive-buffer 65536))
               (http-test-send-request stream (format nil "/~d" (* 4 1024 1024)))
               ;; the head and a little of the body, then gone
               (dotimes (i 1000)
                 (read-byte stream)))
             (with-open-stream (stream (http-test-connect port))
               (http-test-send-request stream "/100000")
               (multiple-value-bind (status headers body complete-p)
                   (http-test-read-response stream)
                 (declare (ignore headers))
                 (values status complete-p body)))))
      (is (eql 200 status))
      (is complete-p)
      (is (equalp (test-pattern 100000) body))))

  (deftest test-http-slow-reader-outlasts-deadlines () ()
    ;; a deferred response read slowly but steadily takes several times
    ;; both the deferred-response deadline and the send timeout: neither
    ;; caps the whole transfer
    (let ((*http-response-timeout-secs* 1)
          (*http-send-timeout-secs* 1)
          (size (* 16 1024 1024)))
      (multiple-value-bind (status complete-p body elapsed)
          (call-with-http-server
           (http-test-handler :defer-secs 0.2)
           #'(lambda (port)
               (with-open-stream (stream (http-test-connect port :receive-buffer 65536))
                 (let ((start (get-internal-real-time)))
                   (http-test-send-request stream (format nil "/~d" size))
                   ;; 256 KiB per 50 ms: 3.2 s
                   (multiple-value-bind (status headers body complete-p)
                       (http-test-read-response stream :piece 262144 :pause 0.05)
                     (declare (ignore headers))
                     (values status complete-p body
                             (/ (- (get-internal-real-time) start)
                                internal-time-units-per-second)))))))
        (is (eql 200 status))
        (is complete-p)
        (is (= size (length body)))
        (is (> elapsed 2)))))

  (deftest test-http-stalled-reader-dropped () ()
    ;; a client that stops reading is dropped once the send timeout
    ;; passes without progress, long before lws's own 15 s deadline
    (let ((*http-send-timeout-secs* 1)
          (size (* 16 1024 1024)))
      (multiple-value-bind (complete-p received)
          (call-with-http-server
           (http-test-handler)
           #'(lambda (port)
               (with-open-stream (stream (http-test-connect port :receive-buffer 65536))
                 (http-test-send-request stream (format nil "/~d" size))
                 ;; stalled for well over the send timeout, then reading
                 ;; as fast as possible: only what the kernels had
                 ;; buffered arrives before the connection ends
                 (sleep 4)
                 (multiple-value-bind (status headers body complete-p)
                     (http-test-read-response stream)
                   (declare (ignore status headers))
                   (values complete-p (length body))))))
        (is-false complete-p)
        (is (< received size))))))
