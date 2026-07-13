;;; SLIME REPL integration for the lws event loop, following the
;;; cl-async-repl (as-repl) pattern: a hook on SLIME's
;;; *SLIME-REPL-EVAL-HOOKS* (swank-listener-hooks contrib) marshals
;;; every REPL form onto the event-loop thread, so single-threaded
;;; loop state (LVGL, cells, ...) can be poked from the normal REPL
;;; with no explicit dispatch calls.
;;;
;;; The loop OWNER (cs-io's START-LWS-EVENT-LOOP, a standalone app)
;;; registers its context with REGISTER-LOOP / UNREGISTER-LOOP;
;;; ENABLE-REPL installs the SLIME hook (opt-in, like
;;; as-repl:start-async-repl), DISABLE-REPL removes it.
;;;
;;; Improvements over as-repl: the caller's stream and printer
;;; specials are captured PER EVAL and rebound on the loop thread, so
;;; output lands in the invoking REPL regardless of how the loop
;;; thread was started; an :AFTER-EVAL hook lets the owner pump its
;;; reactive machinery (cs-io: PROCESS-EVENTS) after each form.
;;;
;;; Threadless builds (32-bit armhf SBCL, no :lws-threads): everything
;;; degrades to direct calls -- there, swank is served from inside the
;;; loop anyway (fd adoption), so REPL forms already run on it.

(defpackage :lws-repl
  (:use :cl)
  (:export #:register-loop #:unregister-loop #:loop-registered-p
           #:sync-action #:sync-eval
           #:enable-repl #:disable-repl #:repl-enabled-p
           #:with-delay
           #:main-thread-repl-p #:yield-main-thread
           #:swank-main-thread-runner-p #:run-on-swank-main-thread))

(in-package :lws-repl)

(defparameter *globals*
  '(*debug-io* *query-io* *terminal-io* *standard-output*
    *standard-input* *error-output* *trace-output*
    *print-array* *print-base* *print-radix*
    *print-case* *print-circle* *print-escape*
    *print-gensym* *print-level* *print-length*
    *print-lines* *print-miser-width* *print-pretty*
    *print-readably* *print-right-margin*)
  "Specials captured from the calling thread and rebound around each
marshalled evaluation (the as-repl *globals* list, minus *package*,
which SYNC-EVAL tracks bidirectionally).")

(defvar *loop-context* nil
  "The registered lws context, or NIL.")
(defvar *loop-thread* nil
  "The thread servicing *LOOP-CONTEXT*.")
(defvar *after-eval-hook* nil
  "Called on the loop thread after each SYNC-EVAL'd form (e.g. cs-io
pumps PROCESS-EVENTS here).")

(defun register-loop (context &key thread after-eval)
  "Register the running loop's CONTEXT (and the THREAD servicing it)
as the target for REPL dispatch.  AFTER-EVAL, if given, runs on the
loop thread after each evaluated form."
  (setf *loop-context* context
        *loop-thread* thread
        *after-eval-hook* after-eval)
  (values))

(defun unregister-loop ()
  (setf *loop-context* nil *loop-thread* nil *after-eval-hook* nil)
  (values))

(defun loop-registered-p ()
  (and *loop-context* t))

(defun sync-action (thunk)
  "Run THUNK on the registered loop thread, wait for it and return its
values; conditions are re-signalled on the caller.  Direct call when
no loop is registered, we are already on the loop thread, or the build
is threadless."
  #-lws-threads (funcall thunk)
  #+lws-threads
  (if (or (null *loop-context*)
          (eq (bt2:current-thread) *loop-thread*))
      (funcall thunk)
      (let ((done (bt2:make-semaphore :name "lws-repl sync action"))
            (result nil)
            (condition nil))
        (lws:defer *loop-context*
                   #'(lambda ()
                       (handler-case
                           (setf result (multiple-value-list (funcall thunk)))
                         (serious-condition (c)
                           (setf condition c)))
                       (bt2:signal-semaphore done)))
        (bt2:wait-on-semaphore done)
        (if condition
            (error condition)
            (values-list result)))))

(defun sync-eval (form)
  "Evaluate FORM on the loop thread with the caller's stream/printer
specials and *PACKAGE* in effect; *PACKAGE* changes propagate back
(IN-PACKAGE works at the REPL).  This is the *SLIME-REPL-EVAL-HOOKS*
hook function."
  (let ((global-values (mapcar #'symbol-value *globals*))
        (package *package*))
    (multiple-value-prog1
        (sync-action
         #'(lambda ()
             (progv (cons '*package* *globals*) (cons package global-values)
               (unwind-protect
                    (multiple-value-prog1
                        (eval form)
                      (when *after-eval-hook*
                        (funcall *after-eval-hook*)))
                 (setf package *package*)))))
      (setf *package* package))))

;;; --- deferred execution (the as:with-delay analog) --------------------

(defun %with-delay (seconds thunk)
  (let ((context (or *loop-context*
                     (error "no lws loop registered")))
        (global-values (mapcar #'symbol-value *globals*))
        (package *package*)
        (after-eval *after-eval-hook*))
    (flet ((run ()
             ;; the caller's streams/printer settings + package, so
             ;; PRINT inside the body lands in the invoking REPL
             (progv (cons '*package* *globals*)
                 (cons package global-values)
               (handler-case
                   (multiple-value-prog1
                       (funcall thunk)
                     (when after-eval (funcall after-eval)))
                 (serious-condition (condition)
                   (format *error-output* "~&;; with-delay error: ~a~%"
                           condition)
                   (force-output *error-output*))))))
      ;; LWS:SCHEDULE must run on the loop thread; LWS:DEFER is the
      ;; thread-safe (and loop-thread-safe) way to get there
      (lws:defer context
                 #'(lambda ()
                     (lws:schedule context
                                   (when (and seconds (plusp seconds))
                                     seconds)
                                   #'run)))))
  (values))

(defmacro with-delay ((&optional (seconds 0)) &body body)
  "Run BODY on the registered loop thread after SECONDS (0/NIL = as
soon as the loop is next serviced) and return IMMEDIATELY -- the
as:with-delay analog.  BODY sees the caller's stream/printer specials
and *PACKAGE*, so printing from it reaches the invoking REPL; its
values are discarded; conditions are reported, never unwound into C.
Under the ENABLE-REPL hook this is how to observe loop progress that
the current REPL form itself would block, e.g.:

  (progn (cl-lvgl.spike:spike-click)
         (lws-repl:with-delay (0.5)
           (print (lvgl:label-get-text ...))))"
  `(%with-delay ,seconds #'(lambda () ,@body)))

;;; --- the SLIME hook --------------------------------------------------

(defun repl-hook-place ()
  "Find (loading the swank-listener-hooks contrib if needed) the
symbol naming SLIME's *SLIME-REPL-EVAL-HOOKS*."
  (let ((swank (or (find-package :swank)
                   (error "SWANK is not loaded"))))
    (or (find-symbol "*SLIME-REPL-EVAL-HOOKS*" swank)
        (progn
          (ignore-errors
           (funcall (find-symbol "SWANK-REQUIRE" swank)
                    :swank-listener-hooks))
          (find-symbol "*SLIME-REPL-EVAL-HOOKS*" swank))
        (error "no *SLIME-REPL-EVAL-HOOKS*: the swank-listener-hooks ~
                contrib is unavailable"))))

(defun enable-repl ()
  "Install the SLIME REPL hook: from now on every form typed at
connected SLIME REPLs is evaluated on the registered loop thread.
Opt-in and reversible (DISABLE-REPL); no-op on threadless builds."
  #+lws-threads
  (progn
    (unless (loop-registered-p)
      (error "no lws loop registered (start it first)"))
    (pushnew 'sync-eval (symbol-value (repl-hook-place))))
  (values))

(defun disable-repl ()
  #+lws-threads
  (ignore-errors
   (setf (symbol-value (repl-hook-place))
         (delete 'sync-eval (symbol-value (repl-hook-place)))))
  (values))

(defun repl-enabled-p ()
  #-lws-threads nil
  #+lws-threads
  (and (find-package :swank)
       (let ((sym (find-symbol "*SLIME-REPL-EVAL-HOOKS*" :swank)))
         (and sym (boundp sym)
              (member 'sync-eval (symbol-value sym))
              t))))

;;; --- slime's run-repl-in-main-thread feature ---------------------------
;;;
;;; Recent slime (Stas Boukarev's "Run repl-thread in the main thread")
;;; parks the process's initial thread in a (:run-on-main-thread FN)
;;; dispatcher after `M-x slime' connects, and runs the REPL there as
;;; its current task -- so GUI code typed at the REPL gets the main
;;; thread.  For a BLOCKING GUI loop that is exactly wrong (the REPL
;;; freezes); the native escape is:
;;;   - queue the loop as the NEXT main-thread task,
;;;   - re-arm swank::*main-thread* (create-repl consumed it via
;;;     SHIFTF) so the dispatcher keeps running,
;;;   - THROW to swank's EXIT-TO-MAIN-THREAD catch: the current eval
;;;     returns as an abort, REPL-LOOP's unwind marks the repl thread
;;;     'aborted, and the NEXT eval auto-spawns "new-repl-thread" off
;;;     the main thread.  The dispatcher then runs the queued loop.
;;; All swank access is via runtime symbol lookup: no dependency, and
;;; graceful degradation on other slime versions.

(defun %swank-symbol (name)
  (let ((package (find-package :swank)))
    (and package (find-symbol name package))))

(defun %swank-value (name)
  (let ((sym (%swank-symbol name)))
    (and sym (boundp sym) (symbol-value sym))))

(defun main-thread-repl-p ()
  "True when the current thread is the process's initial thread AND it
is serving a swank REPL under slime's run-repl-in-main-thread feature
 (so YIELD-MAIN-THREAD applies)."
  #-(and sbcl lws-threads) nil
  #+(and sbcl lws-threads)
  ;; NB native sb-thread objects on BOTH sides: bt2's CURRENT-THREAD
  ;; returns a bordeaux-threads-2 WRAPPER, never EQ to a native thread
  (and (eq sb-thread:*current-thread* (sb-thread:main-thread))
       (%swank-value "*EMACS-CONNECTION*")
       (%swank-symbol "EXIT-TO-MAIN-THREAD")
       t))

(defun swank-main-thread-runner-p ()
  "True when slime's main-thread dispatcher is armed and idle
 (swank::*main-thread* non-NIL), i.e. RUN-ON-SWANK-MAIN-THREAD works."
  (and (%swank-value "*MAIN-THREAD*") t))

(defun run-on-swank-main-thread (thunk)
  "Queue THUNK as a (:run-on-main-thread ...) task for slime's idle
main-thread dispatcher.  Returns immediately."
  (let ((main (or (%swank-value "*MAIN-THREAD*")
                  (error "slime's main-thread dispatcher is not armed")))
        (send (or (%swank-symbol "SEND")
                  (error "no swank::send"))))
    (funcall send main (list :run-on-main-thread thunk)))
  (values))

(defun yield-main-thread (thunk &key (notice "yielding the main thread"))
  "From a main-thread swank REPL (MAIN-THREAD-REPL-P): hand the main
thread over to THUNK and relocate the REPL to a worker thread.  DOES
NOT RETURN -- the current eval finishes as an abort (slime prints
`Evaluation aborted'; NOTICE is printed first to explain), the next
REPL form transparently runs on a freshly spawned repl thread, and
THUNK runs on the freed main thread."
  #-(and sbcl lws-threads)
  (declare (ignore thunk notice))
  #-(and sbcl lws-threads)
  (error "yield-main-thread requires threaded SBCL")
  #+(and sbcl lws-threads)
  (progn
    (unless (main-thread-repl-p)
      (error "not on a main-thread swank REPL"))
    (let ((main sb-thread:*current-thread*) ; native: swank's mailboxes
                                            ; are keyed by native threads
          (send (%swank-symbol "SEND"))
          (main-var (%swank-symbol "*MAIN-THREAD*"))
          (tag (%swank-symbol "EXIT-TO-MAIN-THREAD")))
      (unless (and send main-var tag)
        (error "this slime lacks the run-on-main-thread machinery"))
      (format t "~&;; ~a; the REPL moves to a worker thread ~
                 (the \"aborted\" below is expected)~%" notice)
      (force-output)
      (funcall send main (list :run-on-main-thread thunk))
      ;; create-repl consumed *main-thread* via SHIFTF; re-arm it so
      ;; the dispatcher survives task exits (and future yields work)
      (setf (symbol-value main-var) main)
      (throw tag nil))))
