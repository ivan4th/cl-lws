;;;; -*- Mode: Lisp; -*-

(in-package #:cl-user)

;;; The cscompress gzip/zstd compressor (compress/) and its Lisp
;;; binding, which registers both as HTTP content codings.  A SEPARATE
;;; system from LWS for the same reason as LWS-VNC: only the images
;;; that serve HTTP to browsers load it, and the rest (the console, the
;;; CI test image) must not have to carry libcscompress.so and libzstd.
;;; Same C-library arrangement as the csmb engine in lws.asd.

(defclass compress-c-library (asdf:static-file) ())

(defmethod asdf:input-files ((op asdf:compile-op) (component compress-c-library))
  (let ((dir (asdf:system-relative-pathname :lws-compress "compress/")))
    (append (uiop:directory-files dir "*.c")
            (uiop:directory-files dir "*.h")
            (list (merge-pathnames "Makefile" dir)))))

(defun compress-c-library-path ()
  (asdf:system-relative-pathname
   :lws-compress #+darwin "compress/libcscompress.dylib" #-darwin "compress/libcscompress.so"))

(defmethod asdf:output-files ((op asdf:compile-op) (component compress-c-library))
  (values (list (compress-c-library-path)) t))

(defmethod asdf:perform ((op asdf:compile-op) (component compress-c-library))
  (uiop:run-program
   (list "make" "-C"
         (namestring (asdf:system-relative-pathname :lws-compress "compress/")))
   :output *standard-output*
   :error-output *error-output*))

(defmethod asdf:perform ((op asdf:load-op) (component compress-c-library))
  (uiop:symbol-call :lws '#:load-libcscompress (compress-c-library-path)))

(asdf:defsystem #:lws-compress
  :description "gzip/zstd compression of HTTP responses for the libwebsockets bindings."
  :serial t
  :components ((:compress-c-library "compress-c-library")
               (:file "compress"))
  :depends-on (:lws))
