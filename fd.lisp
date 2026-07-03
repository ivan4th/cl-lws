(in-package :lws)

;;; Watching foreign file descriptors for readability (used to serve
;;; Swank connections from inside the event loop on threadless
;;; builds).  The descriptor is adopted into lws as a raw file; NOTE
;;; that lws takes ownership and closes it when the watch ends, so
;;; callers should pass a dup(2) of the descriptor they care about.

(defstruct (fd-watch (:constructor %make-fd-watch (context fd callback)))
  id
  context
  fd
  callback
  wsi)

(defun watch-fd (context fd callback)
  "Call CALLBACK (no arguments) whenever FD is readable.  lws takes
ownership of FD and closes it when the watch is removed; pass a dup.
Returns an fd-watch for UNWATCH-FD.  Event loop thread only."
  (let* ((watch (%make-fd-watch context fd callback))
         (id (register-lws-object watch)))
    (setf (fd-watch-id watch) id)
    (let ((wsi (%lws-adopt-descriptor-vhost (lws-context-client-vhost context)
                                            :lws-adopt-raw-file-desc fd "cs-fd"
                                            (cffi:null-pointer))))
      (when (cffi:null-pointer-p wsi)
        (unregister-lws-object id)
        (error "watch-fd: lws_adopt_descriptor_vhost failed for fd ~s" fd))
      (setf (fd-watch-wsi watch) wsi)
      (set-wsi-object-id wsi id))
    watch))

(defun unwatch-fd (watch)
  "Stop watching.  Idempotent.  Closes the underlying (dup'ed) fd."
  (when-let ((wsi (shiftf (fd-watch-wsi watch) nil)))
    (%lws-set-timeout wsi :pending-timeout-http-content +lws-to-kill-async+))
  (values))

(define-protocol-callback fd-callback (wsi reason user in len)
  (:lws-callback-raw-rx-file
   (when-let ((watch (wsi-object wsi nil)))
     (funcall (fd-watch-callback watch)))
   0)
  (:lws-callback-raw-close-file
   (when-let ((watch (wsi-object wsi nil)))
     (setf (fd-watch-wsi watch) nil)
     (unregister-lws-object (fd-watch-id watch)))
   0))

(register-lws-protocol "cs-fd" #'(lambda () (cffi:callback fd-callback)))
