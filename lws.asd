;;;; -*- Mode: Lisp; -*-

(in-package #:cl-user)

;; The csmb Modbus engine is a small C library (modbus/) built with
;; make and loaded as a shared library.  cffi-grovel's wrapper
;; mechanism can't express a multi-file C library whose objects are
;; also linked into standalone C unit tests, hence this component.
(defclass modbus-c-library (asdf:static-file) ())

(defmethod asdf:input-files ((op asdf:compile-op) (component modbus-c-library))
  (let ((dir (asdf:system-relative-pathname :lws "modbus/")))
    (append (uiop:directory-files dir "*.c")
            (uiop:directory-files dir "*.h")
            (list (merge-pathnames "Makefile" dir)))))

(defun modbus-c-library-path ()
  (asdf:system-relative-pathname
   :lws #+darwin "modbus/libcsmodbus.dylib" #-darwin "modbus/libcsmodbus.so"))

(defmethod asdf:output-files ((op asdf:compile-op) (component modbus-c-library))
  (values (list (modbus-c-library-path)) t))

(defmethod asdf:perform ((op asdf:compile-op) (component modbus-c-library))
  (uiop:run-program
   (list "make" "-C"
         (namestring (asdf:system-relative-pathname :lws "modbus/")))
   :output *standard-output*
   :error-output *error-output*))

(defmethod asdf:perform ((op asdf:load-op) (component modbus-c-library))
  (uiop:symbol-call :cffi '#:load-foreign-library (modbus-c-library-path)))

(asdf:defsystem #:lws
  :description "libwebsockets bindings for Common Lisp."
  :serial t
  :components ((:file "package")
               (:file "libwebsockets")
               (:cffi-grovel-file "grovel")
               (:cffi-wrapper-file "wrappers")
               (:file "cffi")
               (:file "registry")
               (:file "context")
               (:file "raw")
               (:file "fd")
               (:file "http")
               (:file "mqtt")
               (:modbus-c-library "modbus-c-library")
               (:cffi-grovel-file "modbus-grovel")
               (:file "modbus"))
  :depends-on (:alexandria :i4-diet-utils :iterate :cffi :bordeaux-threads :babel)
  :defsystem-depends-on (:cffi-grovel))
