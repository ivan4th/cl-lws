(in-package :lws)

;;; HTTP server on an explicit vhost.  Static files are served by lws
;;; mounts; everything else lands in LWS_CALLBACK_HTTP on the "cs-http"
;;; protocol and is dispatched to the server's handler function with a
;;; fully materialized http-request (headers are only available during
;;; the HTTP callback, so everything the handler may need is copied up
;;; front; this also allows responses to be deferred).
;;;
;;; The handler receives the request and may respond immediately or
;;; later via HTTP-RESPOND; the response is emitted from the WRITEABLE
;;; callback, headers first, then the body in chunks of
;;; *HTTP-WRITE-CHUNK-SIZE*.  lws calls WRITEABLE only once it has
;;; handed everything written before to the kernel, so each chunk is
;;; progress, and the lws timeout re-armed with every chunk only drops
;;; a connection whose peer stopped reading -- a slow one may take as
;;; long as it needs.
;;;
;;; Bodies are compressed with a content coding the client accepts when
;;; any are registered (REGISTER-HTTP-CONTENT-CODING; the LWS-COMPRESS
;;; system registers zstd and gzip).

(defvar *http-max-body-size* (* 1024 1024)
  "Maximum accepted request body size; larger requests get 413.")

(defvar *http-response-timeout-secs* 75
  "How long lws waits for a deferred response before closing.")

(defvar *http-send-timeout-secs* 30
  "How long a response being sent may make no progress -- lws unable to
take the next body chunk because the peer stopped reading -- before lws
drops the connection.  There is no limit on the total time.")

(defconstant +http-tx-packet-size+ 16384
  "The most lws sends at once on an HTTP connection (the protocol's
tx_packet_size), so that a body chunk usually goes out in one send.")

(defvar *http-write-chunk-size* +http-tx-packet-size+
  "Response body bytes handed to lws per WRITEABLE callback.")

(defstruct (http-server (:constructor %make-http-server (context handler)))
  id
  context
  vhost
  handler
  capture-headers
  mounts-ptr
  foreign-strings
  port)

(defvar *http-servers* (make-hash-table)
  "Maps vhost pointer address -> http-server.  Event loop thread only.")

(defstruct (http-request (:constructor %make-http-request (context server wsi)))
  id
  context
  server
  wsi
  (open-p t)
  method       ;; :get :post :options ...
  path
  query-string
  headers      ;; alist of (lowercase-name . value)
  (body-chunks '())
  (body-size 0)
  body         ;; octets, set on body completion
  dispatched-p
  response     ;; plist (:status :content-type :headers :body)
  response-state ;; nil -> :headers-sent
  (body-offset 0)) ;; response body bytes already handed to lws

;;; request accessors

(defun http-request-header (request name)
  (assoc-value (http-request-headers request) (string-downcase name)
               :test #'equal))

(defun http-request-alive-p (request)
  "True while the underlying connection is still open."
  (and (http-request-open-p request)
       (http-request-wsi request)
       t))

;;; header materialization helpers (HTTP callback only)

(defun %hdr-by-token (wsi token)
  (let ((total (%lws-hdr-total-length wsi token)))
    (when (plusp total)
      (cffi:with-foreign-object (buf :uint8 (+ total 2))
        (let ((n (%lws-hdr-copy wsi buf (+ total 2) token)))
          (when (plusp n)
            (cffi:foreign-string-to-lisp buf :count n)))))))

(defun %hdr-custom (wsi name)
  (let* ((cname (concatenate 'string (string-downcase name) ":"))
         (total (%lws-hdr-custom-length wsi cname (length cname))))
    (when (plusp total)
      (cffi:with-foreign-object (buf :uint8 (+ total 2))
        (let ((n (%lws-hdr-custom-copy wsi buf (+ total 2) cname (length cname))))
          (when (plusp n)
            (cffi:foreign-string-to-lisp buf :count n)))))))

(defun %request-method (wsi)
  (cond ((plusp (%lws-hdr-total-length wsi :wsi-token-get-uri)) :get)
        ((plusp (%lws-hdr-total-length wsi :wsi-token-post-uri)) :post)
        ((plusp (%lws-hdr-total-length wsi :wsi-token-options-uri)) :options)
        (t :other)))

(defun %materialize-request (server wsi in len)
  (let ((request (%make-http-request (http-server-context server) server wsi)))
    (setf (http-request-id request) (register-lws-object request)
          (http-request-method request) (%request-method wsi)
          (http-request-path request)
          (if (or (cffi:null-pointer-p in) (zerop len))
              "/"
              (cffi:foreign-string-to-lisp in :count len))
          (http-request-query-string request)
          (%hdr-by-token wsi :wsi-token-http-uri-args)
          (http-request-headers request)
          (append
           (iter (for (name token) in '(("origin" :wsi-token-origin)
                                        ("authorization" :wsi-token-http-authorization)
                                        ("cookie" :wsi-token-http-cookie)
                                        ("content-type" :wsi-token-http-content-type)
                                        ("accept-encoding" :wsi-token-http-accept-encoding)))
                 (when-let ((value (%hdr-by-token wsi token)))
                   (collect (cons name value))))
           (iter (for name in (http-server-capture-headers server))
                 (when-let ((value (%hdr-custom wsi name)))
                   (collect (cons (string-downcase name) value))))))
    (set-wsi-object-id wsi (http-request-id request))
    request))

;;; server

(defun http-listen (context port handler &key mounts capture-headers iface)
  "Create an HTTP server vhost on PORT.  HANDLER is called with an
http-request for each dynamic request.  MOUNTS is a list of
\(MOUNTPOINT ORIGIN-DIR &optional DEFAULT-FILE) static mounts served
directly by lws.  CAPTURE-HEADERS lists custom header names to
materialize into requests.  Returns an http-server."
  (let* ((server (%make-http-server context handler))
         (id (register-lws-object server)))
    (setf (http-server-id server) id
          (http-server-capture-headers server) capture-headers)
    (flet ((server-string (string)
             (let ((ptr (cffi:foreign-string-alloc string)))
               (push ptr (http-server-foreign-strings server))
               ptr)))
      (when mounts
        (let* ((count (length mounts))
               (arr (cffi:foreign-alloc '(:struct lws-http-mount) :count count)))
          (memset arr '(:struct lws-http-mount) count)
          (iter (for (mountpoint origin default-file) in mounts)
                (for i from 0)
                (let ((entry (cffi:mem-aptr arr '(:struct lws-http-mount) i)))
                  (setf (cffi:foreign-slot-value entry '(:struct lws-http-mount) 'mount-next)
                        (if (< i (1- count))
                            (cffi:mem-aptr arr '(:struct lws-http-mount) (1+ i))
                            (cffi:null-pointer))
                        (cffi:foreign-slot-value entry '(:struct lws-http-mount) 'mountpoint)
                        (server-string mountpoint)
                        (cffi:foreign-slot-value entry '(:struct lws-http-mount) 'origin)
                        (server-string origin)
                        (cffi:foreign-slot-value entry '(:struct lws-http-mount) 'origin-protocol)
                        (cffi:foreign-enum-value 'lws-mount-protocols :lwsmpro-file)
                        (cffi:foreign-slot-value entry '(:struct lws-http-mount) 'mountpoint-len)
                        (length mountpoint))
                  (when default-file
                    (setf (cffi:foreign-slot-value entry '(:struct lws-http-mount) 'def)
                          (server-string default-file)))))
          (setf (http-server-mounts-ptr server) arr)))
      (with-lws-object (info
                        (:struct lws-context-creation-info)
                        port port
                        protocols (lws-context-protocols-ptr context)
                        vhost-name (server-string (format nil "http-~d" id)))
        (when (http-server-mounts-ptr server)
          (setf (cffi:foreign-slot-value info '(:struct lws-context-creation-info) 'mounts)
                (http-server-mounts-ptr server)))
        (when iface
          (setf (cffi:foreign-slot-value info '(:struct lws-context-creation-info) 'iface)
                (server-string iface)))
        (let ((vhost (%lws-create-vhost (lws-context-ctx context) info)))
          (when (cffi:null-pointer-p vhost)
            (http-server-free server)
            (error "http-listen: lws_create_vhost failed for port ~s" port))
          (setf (http-server-vhost server) vhost
                (http-server-port server) (%lws-get-vhost-listen-port vhost)
                (gethash (cffi:pointer-address vhost) *http-servers*) server))))
    server))

(defun http-server-free (server)
  (when (http-server-mounts-ptr server)
    (cffi:foreign-free (http-server-mounts-ptr server)))
  (dolist (ptr (http-server-foreign-strings server))
    (cffi:foreign-string-free ptr))
  (setf (http-server-mounts-ptr server) nil
        (http-server-foreign-strings server) '())
  (unregister-lws-object (http-server-id server)))

(defun http-server-close (server)
  (when-let ((vhost (http-server-vhost server)))
    (remhash (cffi:pointer-address vhost) *http-servers*)
    (setf (http-server-vhost server) nil)
    (%lws-vhost-destroy vhost)
    (http-server-free server))
  (values))

;;; content codings

(defvar *http-content-codings* '()
  "Content codings HTTP-RESPOND may apply to a response body, most
preferred first: a list of (NAME . FUNCTION), NAME a lowercase
Content-Encoding token, FUNCTION taking the body octets and returning
the encoded octets, or NIL to send the body as it is.  Empty unless a
system such as LWS-COMPRESS registers some.")

(defun register-http-content-coding (name function)
  "Make the content coding NAME available to HTTP-RESPOND, encoding with
FUNCTION (see *HTTP-CONTENT-CODINGS*).  A new coding is the least
preferred one; registering NAME again replaces its function in place."
  (let ((name (string-downcase name)))
    (if-let ((existing (assoc name *http-content-codings* :test #'equal)))
      (setf (cdr existing) function)
      (setf *http-content-codings*
            (append *http-content-codings* (list (cons name function))))))
  (values))

(defvar *http-compress-min-size* 1024
  "Bodies shorter than this are sent as they are: encoding them saves
next to nothing.")

(defun http-compressible-content-type-p (content-type)
  "True for the textual media types worth compressing."
  (let* ((semicolon (position #\; content-type))
         (type (string-downcase
                (string-trim " " (subseq content-type 0 semicolon)))))
    (or (starts-with-subseq "text/" type)
        (member type '("application/json" "application/javascript"
                       "application/xml" "image/svg+xml")
                :test #'equal)
        (ends-with-subseq "+json" type)
        (ends-with-subseq "+xml" type))))

(defun %split-and-trim (char string)
  (iter (for start first 0 then (1+ end))
        (for end = (position char string :start start))
        (collect (string-trim '(#\Space #\Tab) (subseq string start end)))
        (while end)))

(defun %parse-qvalue (string)
  "The qvalue STRING (\"0.5\", \"1\", ...) in thousandths, or NIL if it
isn't a well-formed one."
  (let ((len (length string)))
    (when (and (<= 1 len 5)
               (digit-char-p (char string 0))
               (or (= len 1)
                   (and (char= #\. (char string 1))
                        (every #'digit-char-p (subseq string 2)))))
      (let ((value (+ (* 1000 (digit-char-p (char string 0)))
                      (iter (for i from 2 below len)
                            (for scale in '(100 10 1))
                            (sum (* scale (digit-char-p (char string i))))))))
        (when (<= value 1000)
          value)))))

(defun parse-accept-encoding (header)
  "Parse an Accept-Encoding header value into a list of (CODING . Q),
CODING lowercased, Q the qvalue in thousandths (1000 when not given).
Malformed elements are left out."
  (iter (for element in (%split-and-trim #\, header))
        (for (coding . params) = (%split-and-trim #\; element))
        (unless (emptyp coding)
          (let ((q 1000))
            (dolist (param params)
              (let ((eq-pos (position #\= param)))
                (when (and eq-pos
                           (string-equal "q" (string-trim " " (subseq param 0 eq-pos))))
                  (setf q (%parse-qvalue
                           (string-trim " " (subseq param (1+ eq-pos))))))))
            (when q
              (collect (cons (string-downcase coding) q)))))))

(defun http-choose-content-coding (accept-encoding
                                   &optional (codings *http-content-codings*))
  "The entry of CODINGS to encode a response with, given the request's
Accept-Encoding header value ACCEPT-ENCODING (NIL when the request had
none): the coding the client weighs highest, the one earlier in CODINGS
on a tie.  NIL when the client accepts none of them.  A request without
Accept-Encoding gets none either, although RFC 9110 would allow any:
such clients rarely expect it."
  (when accept-encoding
    (let* ((accepted (parse-accept-encoding accept-encoding))
           (wildcard (assoc-value accepted "*" :test #'equal)))
      (flet ((weight (name)
               ;; a coding named explicitly, even with q=0, overrides
               ;; the wildcard
               (or (assoc-value accepted name :test #'equal)
                   (and (equal "gzip" name)
                        (assoc-value accepted "x-gzip" :test #'equal))
                   wildcard
                   0)))
        (iter (with best = nil)
              (with best-weight = 0)
              (for entry in codings)
              (for weight = (weight (car entry)))
              (when (> weight best-weight)
                (setf best entry
                      best-weight weight))
              (finally (return best)))))))

(defun %add-vary (headers field)
  (let ((vary (assoc "vary" headers :test #'string-equal)))
    (cond ((null vary)
           (append headers (list (cons "vary" field))))
          ((or (equal "*" (cdr vary))
               (member field (%split-and-trim #\, (cdr vary)) :test #'string-equal))
           headers)
          (t
           (substitute (cons (car vary) (format nil "~a, ~a" (cdr vary) field))
                       vary headers)))))

(defun %encode-response (request status content-type headers body)
  "Apply a content coding the client accepts to BODY when it is worth it.
Returns the body and the headers to send."
  (when (or (null *http-content-codings*)
            (null content-type)
            (not (http-compressible-content-type-p content-type))
            (member status '(204 304))
            (assoc "content-encoding" headers :test #'string-equal)
            ;; a strong validator must differ between codings (RFC 9110
            ;; 8.8.3), and the handler's names the unencoded body
            (assoc "etag" headers :test #'string-equal))
    (return-from %encode-response (values body headers)))
  ;; the representation depends on Accept-Encoding even when this one
  ;; goes out unencoded
  (let* ((headers (%add-vary headers "Accept-Encoding"))
         (coding (and (>= (length body) *http-compress-min-size*)
                      (http-choose-content-coding
                       (http-request-header request "accept-encoding"))))
         (encoded (when coding
                    (handler-case (funcall (cdr coding) body)
                      (error (condition)
                        (warn "http: ~a encoding failed: ~a" (car coding) condition)
                        nil)))))
    (if (and encoded (< (length encoded) (length body)))
        (values encoded
                (append headers (list (cons "content-encoding" (car coding)))))
        (values body headers))))

;;; responding

(defun http-respond (request &key (status 200) (content-type "text/html")
                               headers body)
  "Set the response for REQUEST and kick its emission.  BODY is a string
or an octet vector.  HEADERS is an alist of (name . value) strings.
The body is compressed with a registered content coding when the client
accepts one, CONTENT-TYPE is textual, the body is at least
*HTTP-COMPRESS-MIN-SIZE* long and STATUS is not 204 or 304, unless
HEADERS already has a Content-Encoding or an ETag.  No-op if the
connection is already gone.  Event loop thread only."
  (when (stringp body)
    (setf body (babel:string-to-octets body :encoding :utf-8)))
  (cond ((not (http-request-alive-p request))
         (dbg "http-respond: request no longer alive"))
        ((http-request-response request)
         (warn "http-respond: response already set"))
        (t
         (multiple-value-bind (body headers)
             (%encode-response request status content-type headers (or body #()))
           (setf (http-request-response request)
                 (list :status status :content-type content-type
                       :headers headers :body body)))
         ;; restart the clock now: the request's own deadline (or the
         ;; deferred-response one) may be nearly spent, and
         ;; %EMIT-RESPONSE-BODY re-arms it only from the first body
         ;; chunk on
         (%lws-set-timeout (http-request-wsi request) :pending-timeout-http-content
                           *http-send-timeout-secs*)
         (%lws-callback-on-writable (http-request-wsi request))))
  (values))

(defun %emit-response-headers (request wsi)
  (let* ((response (http-request-response request))
         (body (getf response :body))
         (buf-size (+ +lws-pre+ 4096))
         (buf (context-scratch (http-request-context request) buf-size))
         (start (cffi:inc-pointer buf +lws-pre+))
         (end (cffi:inc-pointer buf (1- buf-size))))
    (cffi:with-foreign-object (pp :pointer)
      (setf (cffi:mem-ref pp :pointer) start)
      (when (minusp (%lws-add-http-common-headers
                     wsi (getf response :status) (getf response :content-type)
                     (length body) pp end))
        (return-from %emit-response-headers -1))
      (iter (for (name . value) in (getf response :headers))
            (cffi:with-foreign-string (name-ptr (concatenate 'string name ":"))
              (cffi:with-foreign-string ((value-ptr value-len) value)
                (when (minusp (%lws-add-http-header-by-name
                               wsi name-ptr value-ptr (1- value-len) pp end))
                  (return-from %emit-response-headers -1)))))
      (when (minusp (%lws-finalize-write-http-header wsi start pp end))
        (return-from %emit-response-headers -1))
      (setf (http-request-response-state request) :headers-sent)
      (cond ((zerop (length body))
             (%finish-http-transaction request wsi))
            (t
             (%lws-callback-on-writable wsi)
             0)))))

(defun %emit-response-body (request wsi)
  "Hand the next body chunk to lws, the last one as LWS_WRITE_HTTP_FINAL."
  (let* ((body (getf (http-request-response request) :body))
         (offset (http-request-body-offset request))
         (count (min *http-write-chunk-size* (- (length body) offset)))
         (final-p (= (+ offset count) (length body)))
         (buf (context-scratch (http-request-context request) (+ +lws-pre+ count)))
         (payload (cffi:inc-pointer buf +lws-pre+)))
    (copy-octets-to-foreign body payload :start offset :end (+ offset count))
    ;; this callback means the previous chunk is with the kernel
    (%lws-set-timeout wsi :pending-timeout-http-content *http-send-timeout-secs*)
    (cond ((< (%lws-write wsi payload count
                          (if final-p :lws-write-http-final :lws-write-http))
              count)
           -1)
          (final-p
           (%finish-http-transaction request wsi))
          (t
           (setf (http-request-body-offset request) (+ offset count))
           (%lws-callback-on-writable wsi)
           0))))

(defun %finish-http-transaction (request wsi)
  (%request-done request)
  (if (zerop (%lws-http-transaction-completed wsi))
      0
      -1))

(defun %request-done (request)
  (when (http-request-id request)
    (unregister-lws-object (http-request-id request))
    (setf (http-request-id request) nil)))

(defun %dispatch-request (server request wsi)
  (setf (http-request-dispatched-p request) t)
  (handler-bind ((serious-condition
                   #'(lambda (condition)
                       (unless *debug-on-callback-error*
                         (warn "http handler error: ~a" condition)
                         (%lws-return-http-status wsi 500 (cffi:null-pointer))
                         (%request-done request)
                         (return-from %dispatch-request -1)))))
    (funcall (http-server-handler server) request)
    (unless (http-request-response request)
      ;; deferred response: don't let lws time the transaction out early
      (%lws-set-timeout wsi :pending-timeout-http-content
                        *http-response-timeout-secs*))
    0))

;;; the protocol callback

(define-protocol-callback http-callback (wsi reason user in len)
  (:lws-callback-http
   (let ((server (gethash (cffi:pointer-address (%lws-get-vhost wsi))
                          *http-servers*)))
     (cond ((null server)
            (warn "http request on an unknown vhost")
            -1)
           (t
            (let ((request (%materialize-request server wsi in len)))
              (if (eq (http-request-method request) :post)
                  0 ;; wait for the body
                  (%dispatch-request server request wsi)))))))
  (:lws-callback-http-body
   (if-let ((request (wsi-object wsi nil)))
     (cond ((> (+ (http-request-body-size request) len) *http-max-body-size*)
            (%lws-return-http-status wsi 413 (cffi:null-pointer))
            (%request-done request)
            -1)
           (t
            (push (foreign-octets-to-lisp in len) (http-request-body-chunks request))
            (incf (http-request-body-size request) len)
            0))
     0))
  (:lws-callback-http-body-completion
   (if-let ((request (wsi-object wsi nil)))
     (let ((body (make-array (http-request-body-size request)
                             :element-type '(unsigned-byte 8))))
       (iter (with pos = 0)
             (for chunk in (nreverse (http-request-body-chunks request)))
             (replace body chunk :start1 pos)
             (incf pos (length chunk)))
       (setf (http-request-body request) body
             (http-request-body-chunks request) '())
       (%dispatch-request (http-request-server request) request wsi))
     0))
  (:lws-callback-http-writeable
   (if-let ((request (wsi-object wsi nil)))
     (cond ((null (http-request-response request))
            0)
           ((eq (http-request-response-state request) :headers-sent)
            (%emit-response-body request wsi))
           (t
            (%emit-response-headers request wsi)))
     0))
  (:lws-callback-closed-http
   (when-let ((request (wsi-object wsi nil)))
     (setf (http-request-open-p request) nil
           (http-request-wsi request) nil)
     (%request-done request))
   0)
  (:lws-callback-wsi-destroy
   ;; delivered here (default protocol) for every wsi lws destroys;
   ;; see *wsi-destroy-handlers* in registry.lisp
   (handle-wsi-destroy wsi)
   0))

(register-lws-protocol "cs-http" #'(lambda () (cffi:callback http-callback))
                       :default t
                       :tx-packet-size +http-tx-packet-size+)
