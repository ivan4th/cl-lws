(cc-flags #.(format nil "-I~a"
                    (uiop:native-namestring
                     (asdf:system-relative-pathname :lws "modbus/"))))
(include "csmb.h")

(in-package :lws)

(ctype int32-t "int32_t")
(ctype int64-t "int64_t")

;; NOTE: enum members must be groveled with CENUM, not CONSTANT
;; (see grovel.lisp).

(cenum csmb-reg-type
       ((:coil "CSMB_COIL"))
       ((:discrete "CSMB_DISCRETE"))
       ((:holding "CSMB_HOLDING"))
       ((:input "CSMB_INPUT")))

(cenum csmb-ev-type
       ((:span-update "CSMB_EV_SPAN_UPDATE"))
       ((:span-state "CSMB_EV_SPAN_STATE"))
       ((:unit-state "CSMB_EV_UNIT_STATE"))
       ((:write-done "CSMB_EV_WRITE_DONE"))
       ((:conn-state "CSMB_EV_CONN_STATE"))
       ((:slave-write "CSMB_EV_SLAVE_WRITE"))
       ((:log "CSMB_EV_LOG")))

(cenum csmb-span-state
       ((:online "CSMB_ST_ONLINE"))
       ((:stale "CSMB_ST_STALE"))
       ((:offline "CSMB_ST_OFFLINE"))
       ((:uncovered "CSMB_ST_UNCOVERED")))

(cenum csmb-wr-status
       ((:ok "CSMB_WR_OK"))
       ((:exception "CSMB_WR_EXCEPTION"))
       ((:timeout "CSMB_WR_TIMEOUT"))
       ((:verify-failed "CSMB_WR_VERIFY_FAILED"))
       ((:conn-failed "CSMB_WR_CONN_FAILED"))
       ((:unit-disabled "CSMB_WR_UNIT_DISABLED")))

(cenum csmb-conn-state
       ((:connecting "CSMB_CONN_CONNECTING"))
       ((:online "CSMB_CONN_ONLINE"))
       ((:offline "CSMB_CONN_OFFLINE")))

(cenum csmb-conn-error
       ((:none "CSMB_CERR_NONE"))
       ((:connect-failed "CSMB_CERR_CONNECT_FAILED"))
       ((:closed "CSMB_CERR_CLOSED"))
       ((:timeout "CSMB_CERR_TIMEOUT"))
       ((:protocol "CSMB_CERR_PROTOCOL")))

(cenum csmb-log-kind
       ((:exception "CSMB_LOG_EXCEPTION"))
       ((:unit-timeout "CSMB_LOG_UNIT_TIMEOUT")))

(cenum csmb-exception
       ((:none "CSMB_EXC_NONE"))
       ((:illegal-function "CSMB_EXC_ILLEGAL_FUNCTION"))
       ((:illegal-data-address "CSMB_EXC_ILLEGAL_ADDRESS"))
       ((:illegal-data-value "CSMB_EXC_ILLEGAL_VALUE"))
       ((:server-failure "CSMB_EXC_SERVER_FAILURE"))
       ((:acknowledge "CSMB_EXC_ACK"))
       ((:slave-device-busy "CSMB_EXC_BUSY"))
       ((:negative-acknowledge "CSMB_EXC_NACK"))
       ((:memory-parity-error "CSMB_EXC_MEMORY_PARITY"))
       ((:gateway-path-unavailable "CSMB_EXC_GW_PATH"))
       ((:gateway-target-problem "CSMB_EXC_GW_TARGET")))

(cenum csmb-transport-kind
       ((:tcp "CSMB_TR_TCP"))
       ((:serial "CSMB_TR_SERIAL"))
       ((:fd "CSMB_TR_FD")))

(cenum csmb-parity
       ((:none "CSMB_PARITY_NONE"))
       ((:even "CSMB_PARITY_EVEN"))
       ((:odd "CSMB_PARITY_ODD")))

(cenum csmb-blacklist-mode
       ((:none "CSMB_BL_NONE"))
       ((:soft "CSMB_BL_SOFT"))
       ((:hard "CSMB_BL_HARD")))

(cenum csmb-error
       ((:ok "CSMB_OK"))
       ((:overlap "CSMB_EOVERLAP"))
       ((:range "CSMB_ERANGE"))
       ((:no-unit "CSMB_ENOUNIT"))
       ((:bad-type "CSMB_EBADTYPE"))
       ((:too-big "CSMB_ETOOBIG"))
       ((:no-mem "CSMB_ENOMEM"))
       ((:invalid "CSMB_EINVAL"))
       ((:no-span "CSMB_ENOSPAN"))
       ((:exists "CSMB_EEXISTS"))
       ((:transport "CSMB_ETRANSPORT")))

(constant (+csmb-unit-no-verify-write+ "CSMB_UNIT_NO_VERIFY_WRITE"))
(constant (+csmb-unit-fc1516-only+ "CSMB_UNIT_FC1516_ONLY"))
(constant (+csmb-span-always+ "CSMB_SPAN_ALWAYS"))
(constant (+csmb-max-read-flags+ "CSMB_MAX_READ_FLAGS"))
(constant (+csmb-max-read-words+ "CSMB_MAX_READ_WORDS"))
(constant (+csmb-max-write-coils+ "CSMB_MAX_WRITE_COILS"))
(constant (+csmb-max-write-words+ "CSMB_MAX_WRITE_WORDS"))

(cstruct csmb-event "csmb_event"
         (ev-type "type" :type uint8-t)
         (unit "unit" :type uint8-t)
         (reg-type "reg_type" :type uint8-t)
         (state "state" :type uint8-t)
         (exception "exception" :type uint8-t)
         (aux "aux" :type uint8-t)
         (start "start" :type uint16-t)
         (count "count" :type uint16-t)
         (span-id "span_id" :type int32-t)
         (op-id "op_id" :type int64-t)
         (values "values" :type :pointer))

(cstruct csmb-transport "csmb_transport"
         (kind "kind" :type :int)
         (host-or-device "host_or_device" :type :pointer)
         (port "port" :type :int)
         (fd "fd" :type :int)
         (baud "baud" :type :int)
         (data-bits "data_bits" :type uint8-t)
         (parity "parity" :type uint8-t)
         (stop-bits "stop_bits" :type uint8-t)
         (t35-us "t35_us" :type uint32-t))

(cstruct csmb-range "csmb_range"
         (start "start" :type uint16-t)
         (count "count" :type uint16-t))

(cstruct csmb-write-spec "csmb_write_spec"
         (reg-type "reg_type" :type uint8-t)
         (start "start" :type uint16-t)
         (count "count" :type uint16-t)
         (values "values" :type :pointer))
