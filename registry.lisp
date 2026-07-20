(in-package :lws)

;;; Lisp objects referenced from C callbacks are registered here under
;;; integer ids.  The id is stored on the C side as a fake pointer
;;; (cffi:make-pointer id) in the wsi opaque user data / context user
;;; pointer, so no Lisp object addresses ever cross the FFI boundary.

(defun make-lws-lock (name)
  (declare (ignorable name))
  #+lws-threads (bt2:make-lock :name name)
  #-lws-threads nil)

(defmacro with-lws-lock ((lock) &body body)
  #+lws-threads `(bt2:with-lock-held (,lock) ,@body)
  #-lws-threads `(progn ,lock ,@body))

(defvar *lws-object-lock* (make-lws-lock "lws objects"))
(defvar *lws-last-object-id* 0)
(defvar *lws-objects* (make-hash-table))

(defun register-lws-object (object)
  "Register OBJECT and return its integer id."
  (with-lws-lock (*lws-object-lock*)
    (let ((id (incf *lws-last-object-id*)))
      (setf (gethash id *lws-objects*) object)
      id)))

(defun unregister-lws-object (id)
  (with-lws-lock (*lws-object-lock*)
    (remhash id *lws-objects*)))

(defun lws-object-by-id (id &optional (errorp t))
  (let ((object (with-lws-lock (*lws-object-lock*)
                  (gethash id *lws-objects*))))
    (when (and errorp (not object))
      (error "unknown lws object id ~s" id))
    object))

(defun lws-object-from-ptr (ptr &optional (errorp t))
  "Recover a registered object from a fake pointer holding its id."
  (if (cffi:null-pointer-p ptr)
      (progn
        (when errorp
          (error "null lws object pointer"))
        nil)
      (lws-object-by-id (cffi:pointer-address ptr) errorp)))

(defun wsi-object (wsi &optional (errorp t))
  "The Lisp object associated with WSI via opaque user data, or NIL."
  (lws-object-from-ptr (%lws-get-opaque-user-data wsi) errorp))

(defun set-wsi-object-id (wsi id)
  (%lws-set-opaque-user-data wsi (cffi:make-pointer id)))

;;; wsi destruction catch-all.  LWS_CALLBACK_WSI_DESTROY is delivered
;;; unconditionally to the default protocol for every wsi lws tears
;;; down, including paths that skip the role close callbacks (e.g. an
;;; MQTT network wsi closed by validity timeout without cascading to
;;; its mux child).  Interested modules register a handler here; the
;;; handlers must be idempotent no-ops for wsis already cleaned up via
;;; their normal close callbacks.

(defvar *wsi-destroy-handlers* '()
  "Alist of (name . function), each function called with the wsi being
destroyed.  Event loop thread only.")

(defun register-wsi-destroy-handler (name function)
  (let ((entry (assoc name *wsi-destroy-handlers*)))
    (cond (entry
           (setf (cdr entry) function))
          (t
           (push (cons name function) *wsi-destroy-handlers*))))
  (values))

(defun handle-wsi-destroy (wsi)
  (dolist (entry *wsi-destroy-handlers*)
    (funcall (cdr entry) wsi)))

;;; misc foreign helpers

(defun memset (p type &optional (count 1) (val 0))
  (dotimes (i (* count (cffi:foreign-type-size type)))
    (setf (cffi:mem-aref p :uint8 i) val)))

(defmacro with-lws-object ((var type &rest vals) &body body)
  (let ((count 1))
    (when (and (consp type) (numberp (cdr type)))
      (setf count (cdr type) type (car type)))
    `(cffi:with-foreign-object (,var ',type ,count)
       (memset ,var ',type ,count)
       ,@(when vals
           `((setf ,@(iter (for (name value) on vals by #'cddr)
                           (collect `(cffi:foreign-slot-value ,var ',type ',name))
                           (collect value)))))
       ,(maybe-progn body))))

(defmacro with-lws-objects ((&rest defs) &body body)
  (if (null defs)
      (maybe-progn body)
      `(with-lws-object ,(first defs)
         (with-lws-objects ,(rest defs)
           ,@body))))

(defun foreign-octets-to-lisp (ptr len)
  (let ((result (make-array len :element-type '(unsigned-byte 8))))
    (dotimes (i len)
      (setf (aref result i) (cffi:mem-aref ptr :uint8 i)))
    result))

(defun copy-octets-to-foreign (octets ptr &key (start 0) (end (length octets)))
  (iter (for i from start below end)
        (for j from 0)
        (setf (cffi:mem-aref ptr :uint8 j) (aref octets i))))
