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
;;; callback in two phases (headers, then body).

(defvar *http-max-body-size* (* 1024 1024)
  "Maximum accepted request body size; larger requests get 413.")

(defvar *http-response-timeout-secs* 75
  "How long lws waits for a deferred response before closing.")

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
  response-state) ;; nil -> :headers-sent

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
                                        ("content-type" :wsi-token-http-content-type)))
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

;;; responding

(defun http-respond (request &key (status 200) (content-type "text/html")
                               headers body)
  "Set the response for REQUEST and kick its emission.  BODY is a string
or an octet vector.  HEADERS is an alist of (name . value) strings.
No-op if the connection is already gone.  Event loop thread only."
  (when (stringp body)
    (setf body (babel:string-to-octets body :encoding :utf-8)))
  (cond ((not (http-request-alive-p request))
         (dbg "http-respond: request no longer alive"))
        ((http-request-response request)
         (warn "http-respond: response already set"))
        (t
         (setf (http-request-response request)
               (list :status status :content-type content-type
                     :headers headers :body (or body #())))
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
  (let* ((response (http-request-response request))
         (body (getf response :body))
         (len (length body))
         (buf (context-scratch (http-request-context request) (+ +lws-pre+ len)))
         (payload (cffi:inc-pointer buf +lws-pre+)))
    (copy-octets-to-foreign body payload)
    (if (< (%lws-write wsi payload len :lws-write-http-final) len)
        -1
        (%finish-http-transaction request wsi))))

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
                       :default t)
