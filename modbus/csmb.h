/*
 * csmb — Modbus master/slave engine for cl-lws.
 *
 * A libwebsockets-based Modbus/TCP + Modbus/RTU engine designed to be
 * driven from Common Lisp through CFFI.  The master owns connections,
 * reconnection, poll scheduling, read coalescing, a per-span register
 * image, change detection and staleness tracking; the Lisp side is
 * only notified when something changes.  The slave owns a register
 * image and serves master requests entirely in C, surfacing writes as
 * events.
 *
 * Threading: everything here runs on the single lws service thread.
 * No API is thread-safe; marshal calls through the event loop.
 *
 * Event delivery: engines accumulate events in a batch; whenever the
 * batch is non-empty at the end of a C-side activation (lws callback
 * or timer), the notify callback fires ONCE.  The consumer then calls
 * csmb_events_get() to obtain the whole batch (this freezes it; new
 * events accumulate into the next batch) and csmb_events_done() to
 * release it.  Event payload pointers are valid only until
 * csmb_events_done().  Calling engine APIs while draining is allowed;
 * resulting events land in the next batch.
 *
 * Guarantees:
 *  - the first value for a span after subscribe is always published
 *    (after the first successful read);
 *  - subscribing while the transport is down/connecting is fine;
 *  - unit recovery (offline -> online), transport reconnect and unit
 *    re-enable clear span values, so everything republishes;
 *  - a span overlapping a pending write neither updates nor publishes
 *    from read responses until the write completes;
 *  - slave write events fire only for registered writable ranges.
 */

#ifndef CSMB_H_INCLUDED
#define CSMB_H_INCLUDED

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct lws_context;
struct lws_vhost;
struct lws_protocols;

/* ---- register types ---- */

typedef enum csmb_reg_type {
    CSMB_COIL     = 0,
    CSMB_DISCRETE = 1,
    CSMB_HOLDING  = 2,
    CSMB_INPUT    = 3,
    CSMB_NUM_REG_TYPES = 4
} csmb_reg_type_t;

/* ---- limits (match the classic Lisp driver) ---- */

#define CSMB_MAX_READ_FLAGS  2000
#define CSMB_MAX_READ_WORDS  125
#define CSMB_MAX_WRITE_COILS 1968
#define CSMB_MAX_WRITE_WORDS 123
/* max gap of unsubscribed registers a single read may span */
#define CSMB_MAX_DUMMY_BYTES 16   /* = 128 coils or 8 words */
#define CSMB_MAX_PDU  253
#define CSMB_MBAP_LEN 7

/* ---- modbus function codes ---- */

typedef enum csmb_fc {
    CSMB_FC_READ_COILS       = 1,
    CSMB_FC_READ_DISCRETE    = 2,
    CSMB_FC_READ_HOLDING     = 3,
    CSMB_FC_READ_INPUT       = 4,
    CSMB_FC_WRITE_SINGLE_COIL = 5,
    CSMB_FC_WRITE_SINGLE_REG  = 6,
    CSMB_FC_WRITE_MULTI_COILS = 15,
    CSMB_FC_WRITE_MULTI_REGS  = 16
} csmb_fc_t;

/* ---- modbus exception codes ---- */

typedef enum csmb_exc {
    CSMB_EXC_NONE             = 0,
    CSMB_EXC_ILLEGAL_FUNCTION = 1,
    CSMB_EXC_ILLEGAL_ADDRESS  = 2,
    CSMB_EXC_ILLEGAL_VALUE    = 3,
    CSMB_EXC_SERVER_FAILURE   = 4,
    CSMB_EXC_ACK              = 5,
    CSMB_EXC_BUSY             = 6,
    CSMB_EXC_NACK             = 7,
    CSMB_EXC_MEMORY_PARITY    = 8,
    CSMB_EXC_GW_PATH          = 0x0a,
    CSMB_EXC_GW_TARGET        = 0x0b
} csmb_exc_t;

/* ---- error returns (negative) ---- */

enum {
    CSMB_OK        = 0,
    CSMB_EOVERLAP  = -1,   /* intersecting image block range */
    CSMB_ERANGE    = -2,   /* bad start/count */
    CSMB_ENOUNIT   = -3,   /* unknown unit */
    CSMB_EBADTYPE  = -4,   /* e.g. write to a read-only register type */
    CSMB_ETOOBIG   = -5,   /* count exceeds a single-request limit */
    CSMB_ENOMEM    = -6,
    CSMB_EINVAL    = -7,
    CSMB_ENOSPAN   = -8,   /* unknown span id */
    CSMB_EEXISTS   = -9,   /* unit already added */
    CSMB_ETRANSPORT = -10  /* transport setup failed */
};

/* ---- transports ---- */

typedef enum csmb_transport_kind {
    CSMB_TR_TCP    = 0,  /* master: connect; slave: listen */
    CSMB_TR_SERIAL = 1,  /* open device, configure termios, RTU framing */
    CSMB_TR_FD     = 2   /* pre-opened fd (engine takes ownership), RTU framing */
} csmb_transport_kind_t;

typedef enum csmb_parity {
    CSMB_PARITY_NONE = 0,
    CSMB_PARITY_EVEN = 1,
    CSMB_PARITY_ODD  = 2
} csmb_parity_t;

typedef struct csmb_transport {
    int kind;                    /* csmb_transport_kind_t */
    const char *host_or_device;  /* TCP: host / listen iface (NULL = any);
                                    SERIAL: device path.  Copied. */
    int port;                    /* TCP only; slave: 0 = ephemeral */
    int fd;                      /* CSMB_TR_FD only */
    int baud;                    /* SERIAL; 0 = 9600 */
    uint8_t data_bits;           /* SERIAL; 0 = 8 */
    uint8_t parity;              /* csmb_parity_t */
    uint8_t stop_bits;           /* SERIAL; 0 = 1 */
    uint32_t t35_us;             /* RTU inter-frame gap; 0 = from baud,
                                    min 1750us */
} csmb_transport;

/* ---- events ---- */

typedef enum csmb_ev_type {
    CSMB_EV_SPAN_UPDATE = 1, /* span_id, unit, reg_type, start, count, values */
    CSMB_EV_SPAN_STATE  = 2, /* span_id, unit, reg_type, start, count, state */
    CSMB_EV_UNIT_STATE  = 3, /* unit, state (ONLINE/OFFLINE) */
    CSMB_EV_WRITE_DONE  = 4, /* op_id, unit, state=csmb_wr_status, exception,
                                aux = failed request index */
    CSMB_EV_CONN_STATE  = 5, /* state=csmb_conn_state, exception=csmb_conn_error */
    CSMB_EV_SLAVE_WRITE = 6, /* unit, reg_type, start, count, values */
    CSMB_EV_LOG         = 7  /* rate-limited diagnostics: unit, aux=fc,
                                exception, state=csmb_log_kind */
} csmb_ev_type_t;

typedef enum csmb_state {
    CSMB_ST_ONLINE    = 1,
    CSMB_ST_STALE     = 2,
    CSMB_ST_OFFLINE   = 3,
    CSMB_ST_UNCOVERED = 4  /* span not covered by the configured poll-seq */
} csmb_state_t;

typedef enum csmb_wr_status {
    CSMB_WR_OK            = 0,
    CSMB_WR_EXCEPTION     = 1,
    CSMB_WR_TIMEOUT       = 2,
    CSMB_WR_VERIFY_FAILED = 3,
    CSMB_WR_CONN_FAILED   = 4,
    CSMB_WR_UNIT_DISABLED = 5
} csmb_wr_status_t;

typedef enum csmb_conn_state {
    CSMB_CONN_CONNECTING = 1,
    CSMB_CONN_ONLINE     = 2,
    CSMB_CONN_OFFLINE    = 3
} csmb_conn_state_t;

typedef enum csmb_conn_error {
    CSMB_CERR_NONE           = 0,
    CSMB_CERR_CONNECT_FAILED = 1,
    CSMB_CERR_CLOSED         = 2,   /* remote close / device error */
    CSMB_CERR_TIMEOUT        = 3,   /* response timeout closed the conn */
    CSMB_CERR_PROTOCOL       = 4    /* unparseable frames */
} csmb_conn_error_t;

typedef enum csmb_log_kind {
    CSMB_LOG_EXCEPTION    = 1,  /* read exception: unit, aux=fc, exception */
    CSMB_LOG_UNIT_TIMEOUT = 2   /* request to unit timed out */
} csmb_log_kind_t;

typedef struct csmb_event {
    uint8_t  type;       /* csmb_ev_type_t */
    uint8_t  unit;
    uint8_t  reg_type;   /* csmb_reg_type_t */
    uint8_t  state;      /* csmb_state_t / csmb_wr_status_t / csmb_conn_state_t
                            / csmb_log_kind_t depending on type */
    uint8_t  exception;  /* csmb_exc_t / csmb_conn_error_t */
    uint8_t  aux;        /* WRITE_DONE: failed request index; LOG: fc */
    uint16_t start;
    uint16_t count;
    int32_t  span_id;
    int64_t  op_id;
    const uint16_t *values;  /* SPAN_UPDATE / SLAVE_WRITE: COUNT register
                                values (coils: one 0/1 value per coil);
                                valid until csmb_events_done() */
} csmb_event;

/* The notify callback.  Fired at most once per C-side activation when
 * the engine's event batch is non-empty.  OPAQUE is the pointer passed
 * at engine creation.  The callback may call any csmb API on the same
 * engine, including the events_get/events_done pair. */
typedef void (*csmb_notify_cb)(void *opaque);

/* Both csmb_master and csmb_slave begin with a common engine header;
 * csmb_events_get / csmb_events_done accept either. */
size_t csmb_events_get(void *master_or_slave, const csmb_event **out);
void   csmb_events_done(void *master_or_slave);

/* ---- master ---- */

typedef struct csmb_master csmb_master;

/* unit flags */
#define CSMB_UNIT_NO_VERIFY_WRITE 1u  /* accept any non-exception response
                                         to writes (broken echoes) */
#define CSMB_UNIT_FC1516_ONLY     2u  /* never use FC5/FC6 */

/* subscribe flags */
#define CSMB_SPAN_ALWAYS 1u  /* publish every completed read, not only
                                changes */

typedef struct csmb_range { uint16_t start, count; } csmb_range;

typedef struct csmb_write_spec {
    uint8_t reg_type;        /* CSMB_COIL or CSMB_HOLDING */
    uint16_t start, count;
    const uint16_t *values;  /* copied before csmb_master_write returns;
                                coils: one 0/1 value per coil */
} csmb_write_spec;

/* CLIENT_VHOST: the vhost outgoing TCP connections are made through
 * (unused for serial/fd transports, may be NULL then). */
csmb_master *csmb_master_create(struct lws_context *cx,
                                struct lws_vhost *client_vhost,
                                const csmb_transport *tr,
                                csmb_notify_cb notify, void *opaque);
void csmb_master_destroy(csmb_master *m);

int csmb_master_add_unit(csmb_master *m, uint8_t unit,
                         uint32_t request_delay_ms,
                         uint32_t stale_timeout_ms, /* 0 = default 20000 */
                         uint32_t flags);

/* Returns a span id (> 0) or a negative error (CSMB_ETOOBIG if the
 * range doesn't fit in a single read request).  Spans may overlap:
 * intersecting ranges are merged into covering reads by the poll
 * program and each span gets its own values / change detection. */
int32_t csmb_master_subscribe(csmb_master *m, uint8_t unit, int reg_type,
                              uint16_t start, uint16_t count, uint32_t flags);
int csmb_master_unsubscribe(csmb_master *m, int32_t span_id);

/* Force a republish of the span: the next successful read publishes
 * even if the value is unchanged. */
int csmb_master_refresh_span(csmb_master *m, int32_t span_id);

/* Disable/enable polling of one unit.  Disabling keeps subscriptions
 * registered but skips the unit in poll programs and fails its queued
 * writes with CSMB_WR_UNIT_DISABLED; re-enabling clears span values so
 * everything republishes. */
int csmb_master_set_unit_enabled(csmb_master *m, uint8_t unit, int enabled);

/* Explicit poll program for (unit, reg_type): reads are issued exactly
 * as given, in order.  Every subscribed span must be fully contained
 * in one of the ranges; uncovered spans emit a one-shot
 * CSMB_ST_UNCOVERED span-state event and go stale.  N = 0 clears. */
int csmb_master_set_poll_seq(csmb_master *m, uint8_t unit, int reg_type,
                             const csmb_range *ranges, size_t n);

/* Enqueue a write op: N requests executed back-to-back with no
 * interleaved reads to that unit (password sequences).  Returns an op
 * id (> 0) or a negative error.  Values are copied.  Completion is
 * reported with a CSMB_EV_WRITE_DONE event. */
int64_t csmb_master_write(csmb_master *m, uint8_t unit,
                          const csmb_write_spec *reqs, size_t n);

void csmb_master_set_heartbeat(csmb_master *m, uint32_t ms);        /* default 1000 */
void csmb_master_set_response_timeout(csmb_master *m, uint32_t ms); /* default 1000 */

/* ---- slave ---- */

typedef struct csmb_slave csmb_slave;

enum { CSMB_BL_NONE = 0, CSMB_BL_SOFT = 1, CSMB_BL_HARD = 2 };

/* PROTOCOLS: the context's protocols array (needed to create the
 * listener vhost for CSMB_TR_TCP; may be NULL for serial/fd). */
csmb_slave *csmb_slave_create(struct lws_context *cx,
                              const struct lws_protocols *protocols,
                              const csmb_transport *tr,
                              csmb_notify_cb notify, void *opaque);
void csmb_slave_destroy(csmb_slave *s);

/* Actual listen port (after CSMB_TR_TCP with port 0). */
int csmb_slave_listen_port(csmb_slave *s);

int csmb_slave_register_range(csmb_slave *s, uint8_t unit, int reg_type,
                              uint16_t start, uint16_t count, int writable);
/* Safe range: reads return 0 without registration; writes are accepted
 * and discarded. */
int csmb_slave_set_safe_range(csmb_slave *s, uint8_t unit, int reg_type,
                              uint16_t start, uint16_t count);
int csmb_slave_set_values(csmb_slave *s, uint8_t unit, int reg_type,
                          uint16_t start, uint16_t count,
                          const uint16_t *values);
int csmb_slave_blacklist(csmb_slave *s, uint8_t unit, int mode);

/* ---- misc ---- */

/* Test helper: allocate a pty pair; *MASTER_FD gets the master side
 * (wrap with a CSMB_TR_FD transport), SLAVE_PATH the device path of
 * the slave side (open with CSMB_TR_SERIAL).  Returns 0 or -1. */
int csmb_test_openpty(int *master_fd, char *slave_path, size_t path_len);

/* The lws protocol callback implementing protocol "cs-modbus"; look it
 * up with cffi:foreign-symbol-pointer and register it in the protocols
 * array.  (Declared without lws types so this header stays free of
 * libwebsockets.h; the definition uses the exact lws signature.) */

#ifdef __cplusplus
}
#endif

#endif /* CSMB_H_INCLUDED */
