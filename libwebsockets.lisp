(in-package :lws)

(cffi:define-foreign-library libwebsockets
  (:darwin "libwebsockets.dylib")
  #++
  (:default "libwebsockets"))

(cffi:use-foreign-library libwebsockets)

;; The csmb Modbus engine (modbus/, built by lws.asd's
;; MODBUS-C-LIBRARY component).  Loaded by SONAME so that saved images
;; can reopen it through the dynamic-linker search path on deployed
;; systems; a source checkout's build directory is pushed onto
;; CFFI:*FOREIGN-LIBRARY-DIRECTORIES* by LOAD-LIBCSMODBUS and wins
;; when present.
(cffi:define-foreign-library libcsmodbus
  (t (:default "libcsmodbus")))

(defun load-libcsmodbus (build-path)
  (pushnew (uiop:pathname-directory-pathname build-path)
           cffi:*foreign-library-directories*
           :test #'equal)
  (cffi:use-foreign-library libcsmodbus))
