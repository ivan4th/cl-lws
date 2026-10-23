(in-package :lws.tests)

(defun json-like (size)
  (let* ((row "[\"acc-state\",\"Состояние ускорителя\",\"\",\"string\",\"ready\",1,null,null,null,\"state main\",10],")
         (octets (babel:string-to-octets row :encoding :utf-8))
         (result (make-array size :element-type '(unsigned-byte 8))))
    (dotimes (i size result)
      (setf (aref result i) (aref octets (mod i (length octets)))))))

(deftest test-compress-octets-round-trip () ()
  (dolist (encoding '(:gzip :zstd))
    (dolist (size '(0 100 100000))
      (let* ((input (json-like size))
             (compressed (compress-octets encoding input)))
        (is (typep compressed '(simple-array (unsigned-byte 8) (*))))
        (when (= size 100000)
          (is (< (length compressed) (/ size 10))))
        (is (equalp input (decompress-octets encoding compressed size)))
        ;; one octet short of room
        (when (plusp size)
          (is (null (decompress-octets encoding compressed (1- size)))))))
    ;; a non-simple vector is accepted too
    (let ((input (make-array 5000 :element-type '(unsigned-byte 8)
                                  :initial-element 65 :adjustable t)))
      (is (equalp input (decompress-octets encoding (compress-octets encoding input) 5000))))
    (is (null (decompress-octets encoding (json-like 1000) 100000))))
  ;; the gzip and zstd magic numbers
  (is (equalp #(#x1f #x8b) (subseq (compress-octets :gzip (json-like 1000)) 0 2)))
  (is (equalp #(#x28 #xb5 #x2f #xfd) (subseq (compress-octets :zstd (json-like 1000)) 0 4))))

(deftest test-compress-registers-content-codings () ()
  (is (equal '("zstd" "gzip") (mapcar #'car *http-content-codings*))))

#+(and sbcl lws-threads)
(deftest test-http-compression () ()
  (let* ((json (json-like 100000))
         (precompressed (compress-octets :gzip json))
         (requests
           ;; path, Accept-Encoding, expected Content-Encoding, Vary expected
           `(("/json" "gzip, deflate, br, zstd" "zstd" t)
             ("/json" "gzip, deflate" "gzip" t)
             ("/json" nil nil t)
             ("/json" "zstd;q=0, gzip;q=0.8" "gzip" t)
             ("/json" "identity" nil t)
             ("/json" "br" nil t)
             ("/small" "zstd" nil t)
             ("/png" "zstd" nil nil)
             ("/etag" "zstd" nil nil)
             ("/precompressed" "zstd" "gzip" nil)))
         (results
           (call-with-http-server
            #'(lambda (context request)
                (declare (ignore context))
                (let ((path (http-request-path request)))
                  (cond ((equal "/json" path)
                         (http-respond request :content-type "application/json; charset=utf-8"
                                               :body json))
                        ((equal "/small" path)
                         (http-respond request :content-type "application/json"
                                               :body (subseq json 0 100)))
                        ((equal "/png" path)
                         (http-respond request :content-type "image/png" :body json))
                        ((equal "/etag" path)
                         ;; a strong validator of the unencoded body
                         (http-respond request :content-type "application/json"
                                               :headers '(("etag" . "\"v1\""))
                                               :body json))
                        (t
                         ;; the handler's own Content-Encoding is left alone
                         (http-respond request :content-type "application/json"
                                               :headers '(("content-encoding" . "gzip"))
                                               :body precompressed)))))
            #'(lambda (port)
                ;; all over one keep-alive connection
                (with-open-stream (stream (http-test-connect port))
                  (iter (for (path accept-encoding) in requests)
                        (http-test-send-request stream path :accept-encoding accept-encoding)
                        (collect (multiple-value-list (http-test-read-response stream)))))))))
    (iter (for (path accept-encoding expected-encoding vary-p) in requests)
          (for (status headers body complete-p) in results)
          (for encoding = (assoc-value headers "content-encoding" :test #'equal))
          (for vary = (assoc-value headers "vary" :test #'equal))
          (is (eql 200 status))
          (is complete-p)
          (is (equal (princ-to-string (length body))
                     (assoc-value headers "content-length" :test #'equal)))
          (is (equal expected-encoding encoding))
          (is (eq vary-p (and vary (search "Accept-Encoding" vary) t)))
          (cond ((equal "/precompressed" path)
                 (is (equalp precompressed body)))
                (encoding
                 (is (equalp json (decompress-octets (make-keyword (string-upcase encoding))
                                                     body (length json)))))
                ((equal "/small" path)
                 (is (equalp (subseq json 0 100) body)))
                (t
                 (is (equalp json body)))))))
