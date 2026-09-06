(in-package :lws)

;;; Bindings for the csvnc RFB (VNC) server (vnc/libcsvnc).
;;;
;;; The server lives entirely in C inside the lws service loop: Lisp
;;; creates and destroys it and wires two C-to-C paths -- the display
;;; flush hook (cl-lvgl's clvgl_set_flush_hook -> csvnc_blit) and the
;;; key callback (csvnc -> clvgl_push_key_cb) -- through
;;; CFFI:FOREIGN-SYMBOL-POINTER, so no Lisp runs on the pixel or key
;;; path.  Everything here is event-loop-thread-only.

;;; low-level FFI

(cffi:defcstruct csvnc-callbacks
  (key :pointer)
  (pointer :pointer))

(cffi:defcfun ("csvnc_create" %csvnc-create) :pointer
  (cx :pointer)
  (protocols :pointer)
  (iface :pointer)
  (port :int)
  (width :int)
  (height :int)
  (callbacks :pointer)
  (user :pointer))

(cffi:defcfun ("csvnc_listen_port" %csvnc-listen-port) :int
  (server :pointer))

(cffi:defcfun ("csvnc_blit" %csvnc-blit) :void
  (server :pointer)
  (x :int)
  (y :int)
  (w :int)
  (h :int)
  (px :pointer)
  (stride-bytes :int)
  (src-format :int))

(cffi:defcfun ("csvnc_client_count" %csvnc-client-count) :int
  (server :pointer))

(cffi:defcfun ("csvnc_destroy" %csvnc-destroy) :void
  (server :pointer))

(cffi:defcfun ("csvnc_keysym_to_sdl" %csvnc-keysym-to-sdl) :int32
  (keysym :uint32))

;;; the protocol: listener vhosts bind accepted connections to it by name

(register-lws-protocol "cs-vnc"
                       #'(lambda ()
                           (or (cffi:foreign-symbol-pointer
                                "csvnc_lws_protocol_callback")
                               (error "libcsvnc is not loaded"))))

;;; the server wrapper

(defstruct (vnc-server (:constructor %make-vnc-server))
  ptr
  context
  iface)

(defun vnc-server-open (context width height
                        &key (iface "127.0.0.1") (port 0)
                          key-callback pointer-callback
                          (user (cffi:null-pointer)))
  "Start an RFB server on CONTEXT exporting a WIDTH x HEIGHT
framebuffer (initially black) on IFACE:PORT (IFACE NIL = every
interface; PORT 0 = ephemeral, see VNC-SERVER-LISTEN-PORT).
KEY-CALLBACK and POINTER-CALLBACK are C function pointers called from
the loop with USER as their first argument: `void key(void *user,
int32 sdl_key, int32 down)` (e.g. clvgl_push_key_cb) and `void
pointer(void *user, int x, int y, unsigned buttons)`; either may be
NIL.  Pixels arrive through VNC-SERVER-BLIT or, with no Lisp on the
path, through a display flush hook calling VNC-BLIT-FUNCTION with
VNC-SERVER-PTR as its user pointer.  Signals an error when the port
cannot be bound."
  (cffi:with-foreign-objects ((callbacks '(:struct csvnc-callbacks)))
    (setf (cffi:foreign-slot-value callbacks '(:struct csvnc-callbacks) 'key)
          (or key-callback (cffi:null-pointer))
          (cffi:foreign-slot-value callbacks '(:struct csvnc-callbacks)
                                   'pointer)
          (or pointer-callback (cffi:null-pointer)))
    (flet ((create (iface-ptr)
             (%csvnc-create (lws-context-ctx context)
                            (lws-context-protocols-ptr context)
                            iface-ptr port width height callbacks user)))
      (let ((ptr (cond (iface
                        (cffi:with-foreign-string (iface-ptr iface)
                          (create iface-ptr)))
                       (t (create (cffi:null-pointer))))))
        (when (cffi:null-pointer-p ptr)
          (error "vnc-server-open: cannot start a VNC server on ~a:~d"
                 (or iface "*") port))
        (%make-vnc-server :ptr ptr :context context :iface iface)))))

(defun vnc-server-close (server)
  "Drop the clients, close the listener, free the server."
  (when (vnc-server-ptr server)
    (%csvnc-destroy (vnc-server-ptr server))
    (setf (vnc-server-ptr server) nil))
  (values))

(defun vnc-server-listen-port (server)
  "The port the server listens on (after :PORT 0)."
  (%csvnc-listen-port (vnc-server-ptr server)))

(defun vnc-server-client-count (server)
  "The number of clients past the handshake."
  (%csvnc-client-count (vnc-server-ptr server)))

(defun vnc-blit-function ()
  "The C entry point csvnc_blit, to install as a display flush hook
with (VNC-SERVER-PTR server) as the user pointer."
  (or (cffi:foreign-symbol-pointer "csvnc_blit")
      (error "libcsvnc is not loaded")))

(defun vnc-server-blit (server x y width height pixels stride-bytes
                        &key (format :xrgb8888))
  "Copy a WIDTH x HEIGHT block at (X, Y) from the foreign PIXELS (rows
STRIDE-BYTES apart, FORMAT :XRGB8888 or :RGB565) into the server's
framebuffer; what changed is sent to the clients."
  (%csvnc-blit (vnc-server-ptr server) x y width height pixels stride-bytes
               (ecase format
                 (:xrgb8888 0)
                 (:rgb565 1)))
  (values))

(defun vnc-keysym-to-sdl (keysym)
  "The SDL keycode an X KEYSYM is delivered as, or NIL when unmapped."
  (let ((code (%csvnc-keysym-to-sdl keysym)))
    (and (/= code 0) code)))
