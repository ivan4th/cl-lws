(in-package :lws)

;;; context / service loop

(cffi:defcfun (%lws-create-context "lws_create_context" :library libwebsockets)
    :pointer
  (info :pointer))

(cffi:defcfun (%lws-context-destroy "lws_context_destroy" :library libwebsockets)
    :void
  (context :pointer))

(cffi:defcfun (%lws-cancel-service "lws_cancel_service" :library libwebsockets)
    :void
  (context :pointer))

(cffi:defcfun (%lws-service "lws_service" :library libwebsockets)
    :int
  (context :pointer)
  (timeout-ms :int))

(cffi:defcfun (%lws-context-user "lws_context_user") :pointer (context :pointer))

(cffi:defcfun (%lws-get-context "lws_get_context") :pointer (wsi :pointer))

;;; vhosts

(cffi:defcfun (%lws-create-vhost "lws_create_vhost")
    :pointer
  (context :pointer)
  (info :pointer))

(cffi:defcfun (%lws-vhost-destroy "lws_vhost_destroy")
    :void
  (vhost :pointer))

(cffi:defcfun (%lws-get-vhost-listen-port "lws_get_vhost_listen_port")
    :int
  (vhost :pointer))

(cffi:defcfun (%lws-get-vhost "lws_get_vhost") :pointer (wsi :pointer))

;;; connections

(cffi:defcfun (%lws-client-connect-via-info "lws_client_connect_via_info")
    :pointer
  (info :pointer))

(cffi:defcfun (%lws-get-network-wsi "lws_get_network_wsi") :pointer (wsi :pointer))

(cffi:defcfun (%lws-callback-on-writable "lws_callback_on_writable")
    :int
  (wsi :pointer))

(cffi:defcfun (%lws-write "lws_write")
    :int
  (wsi :pointer)
  (buf :pointer)
  (len size-t)
  (protocol lws-write-protocol))

(cffi:defcfun (%lws-set-timeout "lws_set_timeout")
    :void
  (wsi :pointer)
  (reason pending-timeout)
  (secs :int))

(cffi:defcfun (%lws-get-opaque-user-data "lws_get_opaque_user_data")
    :pointer
  (wsi :pointer))

(cffi:defcfun (%lws-set-opaque-user-data "lws_set_opaque_user_data")
    :void
  (wsi :pointer)
  (data :pointer))

(cffi:defcfun (%lws-get-peer-simple "lws_get_peer_simple")
    :string
  (wsi :pointer)
  (name :pointer)
  (namelen size-t))

;;; timers (sul)

(cffi:defcfun (%lws-sul-schedule "lws_sul_schedule")
    :void
  (context :pointer)
  (tsi :int)
  (sul :pointer)
  (cb :pointer)
  (us lws-usec-t))

(cffi:defcfun (%lws-sul-cancel "lws_sul_cancel")
    :void
  (sul :pointer))

;;; http server

(cffi:defcfun (%lws-hdr-total-length "lws_hdr_total_length")
    :int
  (wsi :pointer)
  (token lws-token-indexes))

(cffi:defcfun (%lws-hdr-copy "lws_hdr_copy")
    :int
  (wsi :pointer)
  (dest :pointer)
  (len :int)
  (token lws-token-indexes))

(cffi:defcfun (%lws-hdr-custom-length "lws_hdr_custom_length")
    :int
  (wsi :pointer)
  (name :string)
  (nlen :int))

(cffi:defcfun (%lws-hdr-custom-copy "lws_hdr_custom_copy")
    :int
  (wsi :pointer)
  (dst :pointer)
  (len :int)
  (name :string)
  (nlen :int))

(cffi:defcfun (%lws-add-http-common-headers "lws_add_http_common_headers")
    :int
  (wsi :pointer)
  (code :unsigned-int)
  (content-type :string)
  (content-len lws-filepos-t)
  (p :pointer)  ;; unsigned char **
  (end :pointer))

(cffi:defcfun (%lws-add-http-header-by-name "lws_add_http_header_by_name")
    :int
  (wsi :pointer)
  (name :pointer)   ;; const unsigned char *, must include trailing #\:
  (value :pointer)
  (length :int)
  (p :pointer)
  (end :pointer))

(cffi:defcfun (%lws-finalize-write-http-header "lws_finalize_write_http_header")
    :int
  (wsi :pointer)
  (start :pointer)
  (p :pointer)
  (end :pointer))

(cffi:defcfun (%lws-http-transaction-completed "lws_http_transaction_completed")
    :int
  (wsi :pointer))

(cffi:defcfun (%lws-return-http-status "lws_return_http_status")
    :int
  (wsi :pointer)
  (code :unsigned-int)
  (html-body :string))

;;; fd adoption
;;
;; the third parameter is lws_sock_file_fd_type, a union of int
;; sockfd / int filefd on unix — 4 bytes passed by value like an int

(cffi:defcfun (%lws-adopt-descriptor-vhost "lws_adopt_descriptor_vhost")
    :pointer
  (vh :pointer)
  (type lws-adoption-type)
  (fd :int)
  (vh-prot-name :string)
  (parent :pointer))

;;; mqtt client

(cffi:defcfun (%lws-mqtt-client-send-publish "lws_mqtt_client_send_publish")
    :int
  (wsi :pointer)
  (pub-param :pointer)
  (buf :pointer)
  (len uint32-t)
  (final :int))

(cffi:defcfun (%lws-mqtt-client-send-subcribe "lws_mqtt_client_send_subcribe")
    :int
  (wsi :pointer)
  (sub-param :pointer))

(cffi:defcfun (%lws-mqtt-client-send-unsubcribe "lws_mqtt_client_send_unsubcribe")
    :int
  (wsi :pointer)
  (sub-param :pointer))

;;; logging

(cffi:defcfun (%lws-set-log-level "lws_set_log_level")
    :void
  (level :int)
  (log-emit :pointer))
