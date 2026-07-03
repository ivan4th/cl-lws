(include "libwebsockets.h")

(in-package :lws)

(constant (+lll-user+ "LLL_USER"))
(constant (+lll-err+ "LLL_ERR"))
(constant (+lll-warn+ "LLL_WARN"))
(constant (+lll-notice+ "LLL_NOTICE"))

(constant (+context-port-no-listen+ "CONTEXT_PORT_NO_LISTEN"))
(constant (+lws-server-option-do-ssl-global-init+ "LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT"))
#++(constant (+lws-systate-operational+ "LWS_SYSTATE_OPERATIONAL"))

(ctype size-t "size_t")
(ctype uint8-t "uint8_t")
(ctype uint16-t "uint16_t")
(ctype uint32-t "uint32_t")
(ctype uint64-t "uint64_t")

(ctype gid-t "gid_t")
(ctype uid-t "uid_t")
(ctype lws-usec-t "lws_usec_t")
#+lws-cap
(ctype cap-value-t "cap_value_t")

(cenum lws-system-states-t
       ((:lws-systate-unknown "LWS_SYSTATE_UNKNOWN"))
       ((:lws-systate-context-created "LWS_SYSTATE_CONTEXT_CREATED"))
       ((:lws-systate-initialized "LWS_SYSTATE_INITIALIZED"))
       ((:lws-systate-iface-coldplug "LWS_SYSTATE_IFACE_COLDPLUG"))
       ((:lws-systate-dhcp "LWS_SYSTATE_DHCP"))
       ((:lws-systate-cpd-pre-time "LWS_SYSTATE_CPD_PRE_TIME"))
       ((:lws-systate-time-valid "LWS_SYSTATE_TIME_VALID"))
       ((:lws-systate-cpd-post-time "LWS_SYSTATE_CPD_POST_TIME"))
       ((:lws-systate-policy-valid "LWS_SYSTATE_POLICY_VALID"))
       ((:lws-systate-registered "LWS_SYSTATE_REGISTERED"))
       ((:lws-systate-auth1 "LWS_SYSTATE_AUTH1"))
       ((:lws-systate-auth2 "LWS_SYSTATE_AUTH2"))
       ((:lws-systate-operational "LWS_SYSTATE_OPERATIONAL"))
       ((:lws-systate-policy-invalid "LWS_SYSTATE_POLICY_INVALID"))
       ((:lws-systate-context-destroying "LWS_SYSTATE_CONTEXT_DESTROYING")))

(cenum lws-callback-reason
       ((:lws-callback-protocol-init "LWS_CALLBACK_PROTOCOL_INIT"))
       ((:lws-callback-protocol-destroy "LWS_CALLBACK_PROTOCOL_DESTROY"))
       ((:lws-callback-wsi-create "LWS_CALLBACK_WSI_CREATE"))
       ((:lws-callback-wsi-destroy "LWS_CALLBACK_WSI_DESTROY"))
       ((:lws-callback-wsi-tx-credit-get "LWS_CALLBACK_WSI_TX_CREDIT_GET"))
       ((:lws-callback-openssl-load-extra-client-verify-certs "LWS_CALLBACK_OPENSSL_LOAD_EXTRA_CLIENT_VERIFY_CERTS"))
       ((:lws-callback-openssl-load-extra-server-verify-certs "LWS_CALLBACK_OPENSSL_LOAD_EXTRA_SERVER_VERIFY_CERTS"))
       ((:lws-callback-openssl-perform-client-cert-verification "LWS_CALLBACK_OPENSSL_PERFORM_CLIENT_CERT_VERIFICATION"))
       ((:lws-callback-ssl-info "LWS_CALLBACK_SSL_INFO"))
       ((:lws-callback-openssl-perform-server-cert-verification "LWS_CALLBACK_OPENSSL_PERFORM_SERVER_CERT_VERIFICATION"))
       ((:lws-callback-server-new-client-instantiated "LWS_CALLBACK_SERVER_NEW_CLIENT_INSTANTIATED"))
       ((:lws-callback-http "LWS_CALLBACK_HTTP"))
       ((:lws-callback-http-body "LWS_CALLBACK_HTTP_BODY"))
       ((:lws-callback-http-body-completion "LWS_CALLBACK_HTTP_BODY_COMPLETION"))
       ((:lws-callback-http-file-completion "LWS_CALLBACK_HTTP_FILE_COMPLETION"))
       ((:lws-callback-http-writeable "LWS_CALLBACK_HTTP_WRITEABLE"))
       ((:lws-callback-closed-http "LWS_CALLBACK_CLOSED_HTTP"))
       ((:lws-callback-filter-http-connection "LWS_CALLBACK_FILTER_HTTP_CONNECTION"))
       ((:lws-callback-add-headers "LWS_CALLBACK_ADD_HEADERS"))
       ((:lws-callback-verify-basic-authorization "LWS_CALLBACK_VERIFY_BASIC_AUTHORIZATION"))
       ((:lws-callback-check-access-rights "LWS_CALLBACK_CHECK_ACCESS_RIGHTS"))
       ((:lws-callback-process-html "LWS_CALLBACK_PROCESS_HTML"))
       ((:lws-callback-http-bind-protocol "LWS_CALLBACK_HTTP_BIND_PROTOCOL"))
       ((:lws-callback-http-drop-protocol "LWS_CALLBACK_HTTP_DROP_PROTOCOL"))
       ((:lws-callback-http-confirm-upgrade "LWS_CALLBACK_HTTP_CONFIRM_UPGRADE"))
       ((:lws-callback-established-client-http "LWS_CALLBACK_ESTABLISHED_CLIENT_HTTP"))
       ((:lws-callback-closed-client-http "LWS_CALLBACK_CLOSED_CLIENT_HTTP"))
       ((:lws-callback-receive-client-http-read "LWS_CALLBACK_RECEIVE_CLIENT_HTTP_READ"))
       ((:lws-callback-receive-client-http "LWS_CALLBACK_RECEIVE_CLIENT_HTTP"))
       ((:lws-callback-completed-client-http "LWS_CALLBACK_COMPLETED_CLIENT_HTTP"))
       ((:lws-callback-client-http-writeable "LWS_CALLBACK_CLIENT_HTTP_WRITEABLE"))
       ((:lws-callback-client-http-redirect "LWS_CALLBACK_CLIENT_HTTP_REDIRECT"))
       ((:lws-callback-client-http-bind-protocol "LWS_CALLBACK_CLIENT_HTTP_BIND_PROTOCOL"))
       ((:lws-callback-client-http-drop-protocol "LWS_CALLBACK_CLIENT_HTTP_DROP_PROTOCOL"))
       ((:lws-callback-established "LWS_CALLBACK_ESTABLISHED"))
       ((:lws-callback-closed "LWS_CALLBACK_CLOSED"))
       ((:lws-callback-server-writeable "LWS_CALLBACK_SERVER_WRITEABLE"))
       ((:lws-callback-receive "LWS_CALLBACK_RECEIVE"))
       ((:lws-callback-receive-pong "LWS_CALLBACK_RECEIVE_PONG"))
       ((:lws-callback-ws-peer-initiated-close "LWS_CALLBACK_WS_PEER_INITIATED_CLOSE"))
       ((:lws-callback-filter-protocol-connection "LWS_CALLBACK_FILTER_PROTOCOL_CONNECTION"))
       ((:lws-callback-confirm-extension-okay "LWS_CALLBACK_CONFIRM_EXTENSION_OKAY"))
       ((:lws-callback-ws-server-bind-protocol "LWS_CALLBACK_WS_SERVER_BIND_PROTOCOL"))
       ((:lws-callback-ws-server-drop-protocol "LWS_CALLBACK_WS_SERVER_DROP_PROTOCOL"))
       ((:lws-callback-client-connection-error "LWS_CALLBACK_CLIENT_CONNECTION_ERROR"))
       ((:lws-callback-client-filter-pre-establish "LWS_CALLBACK_CLIENT_FILTER_PRE_ESTABLISH"))
       ((:lws-callback-client-established "LWS_CALLBACK_CLIENT_ESTABLISHED"))
       ((:lws-callback-client-closed "LWS_CALLBACK_CLIENT_CLOSED"))
       ((:lws-callback-client-append-handshake-header "LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER"))
       ((:lws-callback-client-receive "LWS_CALLBACK_CLIENT_RECEIVE"))
       ((:lws-callback-client-receive-pong "LWS_CALLBACK_CLIENT_RECEIVE_PONG"))
       ((:lws-callback-client-writeable "LWS_CALLBACK_CLIENT_WRITEABLE"))
       ((:lws-callback-client-confirm-extension-supported "LWS_CALLBACK_CLIENT_CONFIRM_EXTENSION_SUPPORTED"))
       ((:lws-callback-ws-ext-defaults "LWS_CALLBACK_WS_EXT_DEFAULTS"))
       ((:lws-callback-filter-network-connection "LWS_CALLBACK_FILTER_NETWORK_CONNECTION"))
       ((:lws-callback-ws-client-bind-protocol "LWS_CALLBACK_WS_CLIENT_BIND_PROTOCOL"))
       ((:lws-callback-ws-client-drop-protocol "LWS_CALLBACK_WS_CLIENT_DROP_PROTOCOL"))
       ((:lws-callback-get-thread-id "LWS_CALLBACK_GET_THREAD_ID"))
       ((:lws-callback-add-poll-fd "LWS_CALLBACK_ADD_POLL_FD"))
       ((:lws-callback-del-poll-fd "LWS_CALLBACK_DEL_POLL_FD"))
       ((:lws-callback-change-mode-poll-fd "LWS_CALLBACK_CHANGE_MODE_POLL_FD"))
       ((:lws-callback-lock-poll "LWS_CALLBACK_LOCK_POLL"))
       ((:lws-callback-unlock-poll "LWS_CALLBACK_UNLOCK_POLL"))
       ((:lws-callback-cgi "LWS_CALLBACK_CGI"))
       ((:lws-callback-cgi-terminated "LWS_CALLBACK_CGI_TERMINATED"))
       ((:lws-callback-cgi-stdin-data "LWS_CALLBACK_CGI_STDIN_DATA"))
       ((:lws-callback-cgi-stdin-completed "LWS_CALLBACK_CGI_STDIN_COMPLETED"))
       ((:lws-callback-cgi-process-attach "LWS_CALLBACK_CGI_PROCESS_ATTACH"))
       ((:lws-callback-session-info "LWS_CALLBACK_SESSION_INFO"))
       ((:lws-callback-gs-event "LWS_CALLBACK_GS_EVENT"))
       ((:lws-callback-http-pmo "LWS_CALLBACK_HTTP_PMO"))
       ((:lws-callback-raw-proxy-cli-rx "LWS_CALLBACK_RAW_PROXY_CLI_RX"))
       ((:lws-callback-raw-proxy-srv-rx "LWS_CALLBACK_RAW_PROXY_SRV_RX"))
       ((:lws-callback-raw-proxy-cli-close "LWS_CALLBACK_RAW_PROXY_CLI_CLOSE"))
       ((:lws-callback-raw-proxy-srv-close "LWS_CALLBACK_RAW_PROXY_SRV_CLOSE"))
       ((:lws-callback-raw-proxy-cli-writeable "LWS_CALLBACK_RAW_PROXY_CLI_WRITEABLE"))
       ((:lws-callback-raw-proxy-srv-writeable "LWS_CALLBACK_RAW_PROXY_SRV_WRITEABLE"))
       ((:lws-callback-raw-proxy-cli-adopt "LWS_CALLBACK_RAW_PROXY_CLI_ADOPT"))
       ((:lws-callback-raw-proxy-srv-adopt "LWS_CALLBACK_RAW_PROXY_SRV_ADOPT"))
       ((:lws-callback-raw-proxy-cli-bind-protocol "LWS_CALLBACK_RAW_PROXY_CLI_BIND_PROTOCOL"))
       ((:lws-callback-raw-proxy-srv-bind-protocol "LWS_CALLBACK_RAW_PROXY_SRV_BIND_PROTOCOL"))
       ((:lws-callback-raw-proxy-cli-drop-protocol "LWS_CALLBACK_RAW_PROXY_CLI_DROP_PROTOCOL"))
       ((:lws-callback-raw-proxy-srv-drop-protocol "LWS_CALLBACK_RAW_PROXY_SRV_DROP_PROTOCOL"))
       ((:lws-callback-raw-rx "LWS_CALLBACK_RAW_RX"))
       ((:lws-callback-raw-close "LWS_CALLBACK_RAW_CLOSE"))
       ((:lws-callback-raw-writeable "LWS_CALLBACK_RAW_WRITEABLE"))
       ((:lws-callback-raw-adopt "LWS_CALLBACK_RAW_ADOPT"))
       ((:lws-callback-raw-connected "LWS_CALLBACK_RAW_CONNECTED"))
       ((:lws-callback-raw-skt-bind-protocol "LWS_CALLBACK_RAW_SKT_BIND_PROTOCOL"))
       ((:lws-callback-raw-skt-drop-protocol "LWS_CALLBACK_RAW_SKT_DROP_PROTOCOL"))
       ((:lws-callback-raw-adopt-file "LWS_CALLBACK_RAW_ADOPT_FILE"))
       ((:lws-callback-raw-rx-file "LWS_CALLBACK_RAW_RX_FILE"))
       ((:lws-callback-raw-writeable-file "LWS_CALLBACK_RAW_WRITEABLE_FILE"))
       ((:lws-callback-raw-close-file "LWS_CALLBACK_RAW_CLOSE_FILE"))
       ((:lws-callback-raw-file-bind-protocol "LWS_CALLBACK_RAW_FILE_BIND_PROTOCOL"))
       ((:lws-callback-raw-file-drop-protocol "LWS_CALLBACK_RAW_FILE_DROP_PROTOCOL"))
       ((:lws-callback-timer "LWS_CALLBACK_TIMER"))
       ((:lws-callback-event-wait-cancelled "LWS_CALLBACK_EVENT_WAIT_CANCELLED"))
       ((:lws-callback-child-closing "LWS_CALLBACK_CHILD_CLOSING"))
       ((:lws-callback-connecting "LWS_CALLBACK_CONNECTING"))
       ((:lws-callback-vhost-cert-aging "LWS_CALLBACK_VHOST_CERT_AGING"))
       ((:lws-callback-vhost-cert-update "LWS_CALLBACK_VHOST_CERT_UPDATE"))
       ((:lws-callback-mqtt-new-client-instantiated "LWS_CALLBACK_MQTT_NEW_CLIENT_INSTANTIATED"))
       ((:lws-callback-mqtt-idle "LWS_CALLBACK_MQTT_IDLE"))
       ((:lws-callback-mqtt-client-established "LWS_CALLBACK_MQTT_CLIENT_ESTABLISHED"))
       ((:lws-callback-mqtt-subscribed "LWS_CALLBACK_MQTT_SUBSCRIBED"))
       ((:lws-callback-mqtt-client-writeable "LWS_CALLBACK_MQTT_CLIENT_WRITEABLE"))
       ((:lws-callback-mqtt-client-rx "LWS_CALLBACK_MQTT_CLIENT_RX"))
       ((:lws-callback-mqtt-unsubscribed "LWS_CALLBACK_MQTT_UNSUBSCRIBED"))
       ((:lws-callback-mqtt-drop-protocol "LWS_CALLBACK_MQTT_DROP_PROTOCOL"))
       ((:lws-callback-mqtt-client-closed "LWS_CALLBACK_MQTT_CLIENT_CLOSED"))
       ((:lws-callback-mqtt-ack "LWS_CALLBACK_MQTT_ACK"))
       ((:lws-callback-mqtt-resend "LWS_CALLBACK_MQTT_RESEND"))
       ((:lws-callback-mqtt-unsubscribe-timeout "LWS_CALLBACK_MQTT_UNSUBSCRIBE_TIMEOUT"))
       ((:lws-callback-mqtt-shadow-timeout "LWS_CALLBACK_MQTT_SHADOW_TIMEOUT"))
       ((:lws-callback-user "LWS_CALLBACK_USER")))

(cstruct lws-protocols "struct lws_protocols"
         (name "name" :type :string)
         (callback "callback" :type :pointer)
         (per-session-data-size "per_session_data_size" :type size-t)
         (rx-buffer-size "rx_buffer_size" :type size-t))

(cstruct lws-context-creation-info "struct lws_context_creation_info"
         (iface "iface" :type :string)
         (protocols "protocols" :type :pointer)
         (extensions "extensions" :type :pointer)
         (port "port" :type :int)

         ;; HTTP
         (token-limits "token_limits" :type :pointer)
         (http-proxy-address "http_proxy_address" :type :string)
         (headers "headers" :type :pointer)
         (reject-service-keywords "reject_service_keywords" :type :pointer)
         (pvo "pvo" :type :pointer)
         (log-filepath "log_filepath" :type :string)
         (mounts "mounts" :type :pointer)
         (server-string "server_string" :type :string)
         (error-document-404 "error_document_404" :type :string)
         (http-proxy-port "http_proxy_port" :type :unsigned-int)
         (max-http-header-data2 "max_http_header_data2" :type :unsigned-int)
         (max-http-header-pool2 "max_http_header_pool2" :type :unsigned-int)
         (keepalive-timeout "keepalive_timeout" :type :int)
         (http2-settings "http2_settings" :type uint32-t :count 7)
         (max-http-header-data "max_http_header_data" :type :unsigned-short)
         (max-http-header-pool "max_http_header_pool" :type :unsigned-short)

         ;; TLS
         (ssl-private-key-password "ssl_private_key_password" :type :string)
         (ssl-cert-filepath "ssl_cert_filepath" :type :string)
         (ssl-private-key-filepath "ssl_private_key_filepath" :type :string)
         (ssl-ca-filepath "ssl_ca_filepath" :type :string)
         (ssl-cipher-list "ssl_cipher_list" :type :string)
         (ecdh-curve "ecdh_curve" :type :string)
         (tls1-3-plus-cipher-list "tls1_3_plus_cipher_list" :type :string)
         (server-ssl-cert-mem "server_ssl_cert_mem" :type :pointer)
         (server-ssl-private-key-mem "server_ssl_private_key_mem" :type :pointer)
         (server-ssl-ca-mem "server_ssl_ca_mem" :type :pointer)
         (ssl-options-set "ssl_options_set" :type :long)
         (ssl-options-clear "ssl_options_clear" :type :long)
         (simultaneous-ssl-restriction "simultaneous_ssl_restriction" :type :int)
         (simultaneous-ssl-handshake-restriction "simultaneous_ssl_handshake_restriction" :type :int)
         (ssl-info-event-mask "ssl_info_event_mask" :type :int)
         (server-ssl-cert-mem-len "server_ssl_cert_mem_len" :type :unsigned-int)
	 (server-ssl-private-key-mem-len "server_ssl_private_key_mem_len" :type :unsigned-int)
	 (server-ssl-ca-mem-len "server_ssl_ca_mem_len" :type :unsigned-int)
	 (alpn "alpn" :type :string)

         ;; Client TLS
         (client-ssl-private-key-password "client_ssl_private_key_password" :type :string)
	 (client-ssl-cert-filepath "client_ssl_cert_filepath" :type :string)
	 (client-ssl-cert-mem "client_ssl_cert_mem" :type :pointer)
         (client-ssl-cert-mem-len "client_ssl_cert_mem_len" :type :unsigned-int)
	 (client-ssl-private-key-filepath "client_ssl_private_key_filepath" :type :string)
	 (client-ssl-key-mem "client_ssl_key_mem" :type :pointer)
	 (client-ssl-ca-filepath "client_ssl_ca_filepath" :type :string)
	 (client-ssl-ca-mem "client_ssl_ca_mem" :type :pointer)
	 (client-ssl-cipher-list "client_ssl_cipher_list" :type :string)
	 (client-tls-1-3-plus-cipher-list "client_tls_1_3_plus_cipher_list" :type :string)
	 (ssl-client-options-set "ssl_client_options_set" :type :long)
	 (ssl-client-options-clear "ssl_client_options_clear" :type :long)
	 (client-ssl-ca-mem-len "client_ssl_ca_mem_len" :type :unsigned-int)
	 (client-ssl-key-mem-len "client_ssl_key_mem_len" :type :unsigned-int)
         #-lws-mbedtls
         (provided-client-ssl-ctx "provided_client_ssl_ctx" :type :pointer)
         #+lws-mbedtls
         (mbedtls-client-preload-filepath "mbedtls_client_preload_filepath" :type :string)

         ;; Network options
         (ka-time "ka_time" :type :int)
         (ka-probes "ka_probes" :type :int)
         (ka-interval "ka_interval" :type :int)
         (timeout-secs "timeout_secs" :type :unsigned-int)
         (connect-timeout-secs "connect_timeout_secs" :type :unsigned-int)
         (bind-iface "bind_iface" :type :int)
         (timeout-secs-ah-idle "timeout_secs_ah_idle" :type :unsigned-int)
         (tls-session-timeout "tls_session_timeout" :type uint32-t)
         (tls-session-cache-max "tls_session_cache_max" :type uint32-t)

         (gid "gid" :type gid-t)
         (uid "uid" :type uid-t)
         (options "options" :type uint64-t)
         (user "user" :type :pointer)
         (count-threads "count_threads" :type :unsigned-int)
         (fd-limit-per-thread "fd_limit_per_thread" :type :unsigned-int)
         (vhost-name "vhost_name" :type :string)
         #+lws-plugins
         (plugin-dirs "plugin_dirs" :type :string)
         (external-baggage-free-on-destroy "external_baggage_free_on_destroy" :type :pointer)

         (pt-serv-buf-size "pt_serv_buf_size" :type :unsigned-int)
         (fops "fops" :type :pointer)

         #+lws-socks5
         (socks-proxy-address "socks_proxy_address" :type :string)
         #+lws-socks5
         (socks-proxy-port "socks_proxy_port" :type :unsigned-int)

         #+lws-cap
         (caps "caps" :type cap-value-t :count 4)
         #+lws-cap
         (count-caps "count_caps" :type :char)

         (foreign-loops "foreign_loops" :type :pointer)
         (signal-cb "signal_cb" :type :pointer)
         (pcontext "pcontext" :type :pointer)
         (finalize "finalize" :type :pointer)
         (finalize-arg "finalize_arg" :type :pointer)
         (listen-accept-role "listen_accept_role" :type :string)
         (listen-accept-protocol "listen_accept_protocol" :type :string)
         (pprotocols "pprotocols" :type :pointer)
         (username "username" :type :string)
         (groupname "groupname" :type :string)
         (unix-socket-perms "unix_socket_perms" :type :string)
         (system-ops "system_ops" :type :pointer)
         (retry-and-idle-policy "retry_and_idle_policy" :type :pointer)
         (register-notifier-list "register_notifier_list" :type :pointer)
         #+(and lws-secure-streams lws-secure-streams-static-policy-only)
         (pss-policies "pss_policies" :type :pointer)
         #+(and lws-secure-streams (not lws-secure-streams-static-policy-only))
         (pss-policies-json "pss_policies_json" :type :string)
         #+lws-secure-streams
         (pss-plugins "pss_plugins" :type :pointer)
         #+lws-secure-streams
         (ss-proxy-bind "ss_proxy_bind" :type :string)
         #+lws-secure-streams
         (ss-proxy-address "ss_proxy_address" :type :string)
         #+lws-secure-streams
         (ss-proxy-port "ss_proxy_port" :type uint16-t)

         (rlimit-nofile "rlimit_nofile" :type :int)
         #+lws-peer-limits
         (pl-notify-cb "pl_notify_cb" :type :pointer)
         #+lws-peer-limits
         (ip-limit-ah "ip_limit_ah" :type :unsigned-short)
         #+lws-peer-limits
         (ip-limit-wsi "ip_limit_wsi" :type :unsigned-short)

         #+lws-fault-injection
         (fic "fic" :type lws-fi-ctx-t) ;; TODO: define lws-fi-ctx-t

         ;; System Message Distribution
         #+lws-smd
         (early-smd-cb "early_smd_cb" :type :pointer)
         #+lws-smd
         (early-smd-opaque "early_smd_opaque" :type :pointer)
         #+lws-smd
         (early-smd-class-filter "early_smd_class_filter" :type lws-smd-class-t)
         #+lws-smd
         (smd-ttl-us "smd_ttl_us" :type lws-usec-t)
         #+lws-smd
         (smd-dequeue-depth "smd_dequeue_depth" :type uint16-t)

         #+lws-sys-metrics
         (metrics-policies "metrics_policies" :type :pointer)
         #+lws-sys-metrics
         (metrics-prefix "metrics_prefix" :type :string)

         (fo-listen-queue "fo_listen_queue" :type :int)

         (event-lib-custom "event_lib_custom" :type :pointer)

         #+lws-tls-jit-trust
         (jitt-cache-max-footprint "jitt_cache_max_footprint" :type size-t)
         #+lws-tls-jit-trust
         (vh-idle-grace-ms "vh_idle_grace_ms" :type :int)

         (log-cx "log_cx" :type :pointer)

         (http-nsc-filepath "http_nsc_filepath" :type :string)
         (http-nsc-heap-max-footprint "http_nsc_heap_max_footprint" :type size-t)
         (http-nsc-heap-max-items "http_nsc_heap_max_items" :type size-t)
         (http-nsc-heap-max-payload "http_nsc_heap_max_payload" :type size-t))

(cstruct lws-client-connect-info "struct lws_client_connect_info"
         (context "context" :type :pointer)
         (address "address" :type :string)
         (port "port" :type :int)
         (ssl-connection "ssl_connection" :type :int)
         (path "path" :type :string)
         (host "host" :type :string)
         (origin "origin" :type :string)
         (protocol "protocol" :type :string)
         (ietf-version-or-minus-one "ietf_version_or_minus_one" :type :int)
         (userdata "userdata" :type :pointer)
         (client-exts "client_exts" :type :pointer)
         (method "method" :type :string)
         (parent-wsi "parent_wsi" :type :pointer)
         (uri-replace-from "uri_replace_from" :type :string)
         (uri-replace-to "uri_replace_to" :type :string)
         (vhost "vhost" :type :pointer)
         (pwsi "pwsi" :type :pointer)
         (iface "iface" :type :string)
         (local-port "local_port" :type :int)
         (local-protocol-name "local_protocol_name" :type :string)
         (alpn "alpn" :type :string)
         (opaque-user-data "opaque_user_data" :type :pointer)
         (retry-and-idle-policy "retry_and_idle_policy" :type :pointer)
         (manual-initial-tx-credit "manual_initial_tx_credit" :type :int)
         (sys-tls-client-cert "sys_tls_client_cert" :type uint8-t)
         (priority "priority" :type uint8-t)
         (mqtt-cp "mqtt_cp" :type :pointer)
         (fi-wsi-name "fi_wsi_name" :type :string)
         (keep-warm-secs "keep_warm_secs" :type uint16-t)
         (log-cx "log_cx" :type :pointer))

(cstruct lws-dll2 "struct lws_dll2"
         (prev "prev" :type :pointer)
         (next "next" :type :pointer)
         (owner "owner" :type :pointer))

(cstruct lws-dll2-owner "lws_dll2_owner_t"
         (prev "tail" :type :pointer)
         (next "head" :type :pointer)
         (count "count" :type uint32-t))

(cstruct lws-state-notify-link "lws_state_notify_link_t"
         (list "list" :type (:struct lws-dll2))
         (notify-cb "notify_cb" :type :pointer)
         (name "name" :type :string))

(cstruct lws-state-manager "lws_state_manager_t"
         (notify-list "notify_list" :type (:struct lws-dll2-owner))
         (context "context" :type :pointer)
         (parent "parent" :type :pointer)
         (state-names "state_names" :type :pointer)
         (name "name" :type :string)
         (state "state" :type :int))

(cstruct lws-retry-bo "struct lws_retry_bo"
         (retry-ms-table "retry_ms_table" :type :pointer)
         (retry-ms-table-count "retry_ms_table_count" :type uint16-t)
         (conceal-count "conceal_count" :type uint16-t)
         (secs-since-valid-ping "secs_since_valid_ping" :type uint16-t)
         (secs-since-valid-hangup "secs_since_valid_hangup" :type uint16-t)
         (jitter-percent "jitter_percent" :type uint8-t))

(cenum lws-mqtt-qos-levels-t
       ((:qos0 "QOS0"))
       ((:qos1 "QOS1"))
       ((:qos2 "QOS2"))
       ((:reserved-qos-level "RESERVED_QOS_LEVEL"))
       ((:failure-qos-level "FAILURE_QOS_LEVEL")))

(cstruct lws-mqtt-client-connect-param "lws_mqtt_client_connect_param_t"
         (client-id "client_id" :type :string)
         (keep-alive "keep_alive" :type uint16-t)
         ;; bit fields:
         ;; clean_start
         ;; client_id_nofree
         ;; username_nofree
         ;; password_nofree
         (will-topic "will_param.topic" :type :string)
         (will-message "will_param.message" :type :string)
         (will-qos "will_param.qos" :type lws-mqtt-qos-levels-t)
         (will-retain "will_param.retain" :type uint8-t)
         (birth-topic "birth_param.topic" :type :string)
         (birth-message "birth_param.message" :type :string)
         (birth-qos "birth_param.qos" :type lws-mqtt-qos-levels-t)
         (birth-retain "birth_param.retain" :type uint8-t)
         (username "username" :type :string)
         (password "password" :type :string)
         (aws-iot "aws_iot" :type :string))

(cstruct lws-mqtt-publish-param "lws_mqtt_publish_param_t"
         (topic "topic" :type :pointer)
         (topic-len "topic_len" :type uint16-t)
         (payload "payload" :type :pointer)
         (payload-len "payload_len" :type uint32-t)
         (payload-pos "payload_pos" :type uint32-t)
         (qos "qos" :type lws-mqtt-qos-levels-t)
         (packet-id "packet_id" :type uint16-t)
         ;; bit fields:
         ;; dup
         ;; retain
         )

;; TBD: rename to lws-mqtt-topic-elem

(cstruct topic-elem "lws_mqtt_topic_elem_t"
         (name "name" :type :string)
         (qos "qos" :type lws-mqtt-qos-levels-t)
         (acked "acked" :type uint8-t))

(cstruct lws-mqtt-subscribe-param "lws_mqtt_subscribe_param_t"
         (num-topics "num_topics" :type uint32-t)
         (topic "topic" :type :pointer)
         (packet_id "packet_id" :type uint16-t))

;;; --- raw / http / vhost support ---

(constant (+lws-pre+ "LWS_PRE"))
(constant (+lws-server-option-explicit-vhosts+ "LWS_SERVER_OPTION_EXPLICIT_VHOSTS"))
(constant (+lws-server-option-adopt-apply-listen-accept-config+
           "LWS_SERVER_OPTION_ADOPT_APPLY_LISTEN_ACCEPT_CONFIG"))
(constant (+lws-to-kill-async+ "LWS_TO_KILL_ASYNC"))

;; enum members can't be groveled with CONSTANT (the generated code
;; checks for them with #ifdef)
(cenum lws-mount-protocols
       ((:lwsmpro-http "LWSMPRO_HTTP"))
       ((:lwsmpro-file "LWSMPRO_FILE"))
       ((:lwsmpro-callback "LWSMPRO_CALLBACK")))

(cenum lws-adoption-type
       ((:lws-adopt-raw-file-desc "LWS_ADOPT_RAW_FILE_DESC"))
       ((:lws-adopt-http "LWS_ADOPT_HTTP"))
       ((:lws-adopt-socket "LWS_ADOPT_SOCKET")))

(ctype lws-filepos-t "lws_filepos_t")

(cenum pending-timeout
       ((:no-pending-timeout "NO_PENDING_TIMEOUT"))
       ((:pending-timeout-http-content "PENDING_TIMEOUT_HTTP_CONTENT")))

(cenum lws-write-protocol
       ((:lws-write-http "LWS_WRITE_HTTP"))
       ((:lws-write-http-final "LWS_WRITE_HTTP_FINAL")))

(cenum lws-token-indexes
       ((:wsi-token-get-uri "WSI_TOKEN_GET_URI"))
       ((:wsi-token-post-uri "WSI_TOKEN_POST_URI"))
       ((:wsi-token-options-uri "WSI_TOKEN_OPTIONS_URI"))
       ((:wsi-token-origin "WSI_TOKEN_ORIGIN"))
       ((:wsi-token-http-authorization "WSI_TOKEN_HTTP_AUTHORIZATION"))
       ((:wsi-token-http-cookie "WSI_TOKEN_HTTP_COOKIE"))
       ((:wsi-token-http-content-length "WSI_TOKEN_HTTP_CONTENT_LENGTH"))
       ((:wsi-token-http-content-type "WSI_TOKEN_HTTP_CONTENT_TYPE"))
       ((:wsi-token-http-uri-args "WSI_TOKEN_HTTP_URI_ARGS")))

(cstruct lws-sorted-usec-list "lws_sorted_usec_list_t"
         (list "list" :type (:struct lws-dll2))
         (us "us" :type lws-usec-t)
         (cb "cb" :type :pointer)
         (latency-us "latency_us" :type uint32-t))

(cstruct lws-http-mount "struct lws_http_mount"
         (mount-next "mount_next" :type :pointer)
         (mountpoint "mountpoint" :type :string)
         (origin "origin" :type :string)
         (def "def" :type :string)
         (protocol "protocol" :type :string)
         (origin-protocol "origin_protocol" :type :unsigned-char)
         (mountpoint-len "mountpoint_len" :type :unsigned-char))
