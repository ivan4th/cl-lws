(in-package :lws)

;;; One-shot gzip / zstd compression done in C by libcscompress
;;; (compress/, zlib + libzstd), and the HTTP content codings using it.
;;; Loading this file registers zstd (preferred: cheaper on the CPU and
;;; smaller output) and gzip with HTTP-RESPOND.

(cffi:defcenum compress-encoding
  (:gzip 1)
  (:zstd 2))

(cffi:defcfun (%cscompress-bound "cscompress_bound" :library libcscompress) size-t
  (encoding compress-encoding)
  (len size-t))

(cffi:defcfun (%cscompress "cscompress" :library libcscompress) size-t
  (encoding compress-encoding)
  (level :int)
  (src :pointer)
  (len size-t)
  (dst :pointer)
  (cap size-t))

(cffi:defcfun (%cscompress-decompress "cscompress_decompress" :library libcscompress)
    size-t
  (encoding compress-encoding)
  (src :pointer)
  (len size-t)
  (dst :pointer)
  (cap size-t))

(defvar *gzip-level* 1
  "zlib level (1..9) of gzip-encoded HTTP responses.  The fastest: the
control computers are slow.")

(defvar *zstd-level* 1
  "zstd level (1..19, see CSCOMPRESS_ZSTD_MAX_LEVEL) of zstd-encoded HTTP
responses.")

(defun %shareable-octets (octets)
  (if (typep octets '(simple-array (unsigned-byte 8) (*)))
      octets
      (let ((copy (cffi:make-shareable-byte-vector (length octets))))
        (replace copy octets)
        copy)))

(defun compress-octets (encoding octets &key level)
  "Compress OCTETS with ENCODING (:GZIP or :ZSTD) at LEVEL (default
*GZIP-LEVEL* / *ZSTD-LEVEL*).  Returns a new octet vector, or NIL if
the compressor failed."
  (let* ((src (%shareable-octets octets))
         (len (length src))
         (level (or level (ecase encoding
                            (:gzip *gzip-level*)
                            (:zstd *zstd-level*))))
         (cap (%cscompress-bound encoding len)))
    (unless (zerop cap)
      (let* ((dst (cffi:make-shareable-byte-vector cap))
             (size (cffi:with-pointer-to-vector-data (src-ptr src)
                     (cffi:with-pointer-to-vector-data (dst-ptr dst)
                       (%cscompress encoding level src-ptr len dst-ptr cap)))))
        (unless (zerop size)
          (subseq dst 0 size))))))

(defun decompress-octets (encoding octets max-size)
  "Decompress OCTETS, compressed with ENCODING, into at most MAX-SIZE
octets.  Returns a new octet vector, or NIL on corrupt or truncated
input or output longer than MAX-SIZE.  For tests and diagnostics."
  (let* ((src (%shareable-octets octets))
         (dst (cffi:make-shareable-byte-vector max-size))
         (size (cffi:with-pointer-to-vector-data (src-ptr src)
                 (cffi:with-pointer-to-vector-data (dst-ptr dst)
                   (%cscompress-decompress encoding src-ptr (length src)
                                           dst-ptr max-size)))))
    ;; (size_t)-1 on failure
    (when (<= size max-size)
      (subseq dst 0 size))))

(register-http-content-coding "zstd" #'(lambda (octets) (compress-octets :zstd octets)))
(register-http-content-coding "gzip" #'(lambda (octets) (compress-octets :gzip octets)))
