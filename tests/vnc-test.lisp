(in-package :lws.tests)

;;; The csvnc binding: server lifecycle on a context, a 565 blit seen
;;; through the RFB client, client info, dropping clients.

(deftest test-vnc-binding-create-destroy () ()
  ;; two servers in a row on the same context: the first's port is
  ;; free again once it is closed, nothing leaks a vhost
  (call-with-test-context
   #'(lambda (context)
       (let* ((server (vnc-server-open context 64 48 :port 0))
              (port (vnc-server-listen-port server)))
         (is (plusp port))
         (is (= 0 (vnc-server-client-count server)))
         (is (null (vnc-server-client-info server 0)))
         ;; the same port again while it is served: refused
         (signals error (vnc-server-open context 64 48 :port port))
         (vnc-server-close server)
         (is (null (vnc-server-ptr server)))
         ;; idempotent
         (vnc-server-close server)
         (schedule context nil
                   #'(lambda ()
                       (let ((again (vnc-server-open context 64 48 :port port)))
                         (is (= port (vnc-server-listen-port again)))
                         (vnc-server-close again)
                         (stop-context context))))))))

(deftest test-vnc-blit-565-expansion () ()
  ;; a 565 blit reaches a Raw client as bit-replicated XRGB; the
  ;; client info follows the connection
  (let ((pixels (cffi:foreign-alloc :uint16 :count 8))
        (seen '())
        (info-before nil)
        (info-after nil))
    (unwind-protect
         (call-with-test-context
          #'(lambda (context)
              (let* ((server (vnc-server-open context 4 2 :port 0))
                     (port (vnc-server-listen-port server))
                     (client nil))
                ;; 0xf800 -> ff0000, 0x07e0 -> 00ff00, 0x001f -> 0000ff,
                ;; 0x8410 -> 848284, then black
                (iter (for value in '(#xf800 #x07e0 #x001f #x8410 0 0 0 0))
                      (for i from 0)
                      (setf (cffi:mem-aref pixels :uint16 i) value))
                (vnc-server-blit server 0 0 4 2 pixels 8 :format :rgb565)
                (setf client
                      (rfb-connect
                       context "127.0.0.1" port
                       :on-established
                       #'(lambda (client)
                           (is (= 4 (rfb-client-width client)))
                           (is (= 2 (rfb-client-height client)))
                           (is (equal "EACS console" (rfb-client-name client)))
                           (is (= 1 (vnc-server-client-count server)))
                           (setf info-before (vnc-server-client-info server 0))
                           (rfb-request-update client :incremental nil))
                       :on-update
                       #'(lambda (client rects)
                           (setf seen (iter (for i below 8)
                                            (collect (aref (rfb-client-frame client) i))))
                           (setf info-after (vnc-server-client-info server 0))
                           (is (every #'(lambda (rect) (= 0 (fifth rect))) rects))
                           (vnc-server-drop-clients server))
                       :on-closed
                       #'(lambda (client)
                           (declare (ignore client))
                           (vnc-server-close server)
                           (stop-context context)))))))
      (cffi:foreign-free pixels))
    (is (equal '(#xff0000 #x00ff00 #x0000ff #x848284 0 0 0 0) seen))
    (is (getf info-before :established-p))
    (is (eq :raw (getf info-before :encoding)))
    (is (= 32 (getf info-before :bpp)))
    (is (= 0 (getf info-after :pending-rects)))
    (is (= 0 (getf info-after :queued-bytes)))))
