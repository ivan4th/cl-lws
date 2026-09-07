;;;; -*- Mode: Lisp; -*-

(in-package #:cl-user)

;;; The csvnc RFB (VNC) server (vnc/) and its Lisp binding, a SEPARATE
;;; system from LWS on purpose: only the LVGL console serves VNC, and
;;; every other image that loads LWS (the control systems, the CI test
;;; image) must not have to carry libcsvnc.so.  Same C-library
;;; arrangement as the csmb engine in lws.asd.

(defclass vnc-c-library (asdf:static-file) ())

(defmethod asdf:input-files ((op asdf:compile-op) (component vnc-c-library))
  (let ((dir (asdf:system-relative-pathname :lws-vnc "vnc/")))
    (append (uiop:directory-files dir "*.c")
            (uiop:directory-files dir "*.h")
            (list (merge-pathnames "Makefile" dir)))))

(defun vnc-c-library-path ()
  (asdf:system-relative-pathname
   :lws-vnc #+darwin "vnc/libcsvnc.dylib" #-darwin "vnc/libcsvnc.so"))

(defmethod asdf:output-files ((op asdf:compile-op) (component vnc-c-library))
  (values (list (vnc-c-library-path)) t))

(defmethod asdf:perform ((op asdf:compile-op) (component vnc-c-library))
  (uiop:run-program
   (list "make" "-C"
         (namestring (asdf:system-relative-pathname :lws-vnc "vnc/")))
   :output *standard-output*
   :error-output *error-output*))

(defmethod asdf:perform ((op asdf:load-op) (component vnc-c-library))
  (uiop:symbol-call :lws '#:load-libcsvnc (vnc-c-library-path)))

(asdf:defsystem #:lws-vnc
  :description "The csvnc VNC server for the libwebsockets bindings."
  :serial t
  :components ((:vnc-c-library "vnc-c-library")
               (:file "vnc")
               (:file "vnc-client"))
  :depends-on (:lws))
