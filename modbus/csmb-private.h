/* Internal structures shared across the csmb engine sources.
 *
 * This header must stay free of libwebsockets.h so that the pure-logic
 * translation units (codec, sched, image, event) and their unit tests
 * build without lws.  Files that talk to lws (master, slave, transport,
 * lws glue) include libwebsockets.h themselves.
 */

#ifndef CSMB_PRIVATE_H_INCLUDED
#define CSMB_PRIVATE_H_INCLUDED

#include <stdlib.h>
#include <string.h>
#include "csmb.h"

typedef int64_t csmb_usec_t;

/* ---- event batching (csmb-event.c) ----
 *
 * Events accumulate in a growable array with payloads in a growable
 * arena.  Because the arena may realloc while a batch accumulates,
 * csmb_event.values holds an arena OFFSET (as a fake pointer) until
 * csmb_events_get() freezes the batch and fixes the pointers up.
 * A frozen (draining) batch is kept aside so that engine calls made
 * from inside the notify callback append to a fresh batch.
 */

typedef struct csmb_event_buf {
    csmb_event *events;
    size_t nevents, cap;
    uint8_t *arena;
    size_t arena_used, arena_cap;
} csmb_event_buf;

/* Common engine header.  MUST be the first member of csmb_master and
 * csmb_slave (csmb_events_get/done cast through it). */
typedef struct csmb_engine {
    struct lws_context *cx;
    csmb_notify_cb notify;
    void *opaque;
    int in_notify;
    int destroyed;
    csmb_event_buf pending;   /* accumulating batch */
    csmb_event_buf draining;  /* frozen batch handed to the consumer */
    int drain_active;
    int consumed_in_notify;   /* consumer called events_get during notify */
} csmb_engine;

void csmb_engine_init(csmb_engine *e, struct lws_context *cx,
                      csmb_notify_cb notify, void *opaque);
void csmb_engine_free(csmb_engine *e);

/* Append an event; if PAYLOAD is non-NULL, copy N uint16 values into
 * the arena and make ev->values refer to them.  Returns 0 or
 * CSMB_ENOMEM. */
int csmb_event_add(csmb_engine *e, const csmb_event *ev,
                   const uint16_t *payload, size_t n);

/* Fire the notify callback once if the pending batch is non-empty and
 * we are not already inside a notify.  Call at the end of every
 * C-side activation (lws callback, sul timer, public API call that
 * can emit events synchronously). */
void csmb_event_flush(csmb_engine *e);

/* convenience */
#define CSMB_ZERO_EVENT(ev) csmb_event ev; memset(&ev, 0, sizeof(ev))

/* ---- codec: PDU encode/decode, framing, CRC16 (csmb-codec.c) ---- */

/* CRC16 (Modbus): init 0xFFFF, reflected poly 0xA001, table-driven. */
uint16_t csmb_crc16(const uint8_t *p, size_t len);

/* Resumable frame parsers.  Feed functions consume from *IN (advancing
 * it) and decrement *LEN; when a frame completes mid-buffer the leftover
 * bytes stay unconsumed for the next call.  After CSMB_PR_FRAME the
 * caller inspects the public result fields, then resets/reuses the
 * parser before feeding the next frame. */
typedef enum csmb_parse_ret {
    CSMB_PR_FRAME     = 0,   /* a full frame is available */
    CSMB_PR_NEED_MORE = 1,   /* input exhausted, feed more */
    CSMB_PR_BAD       = -1    /* framing error; resync required */
} csmb_parse_ret;

/* Modbus/TCP MBAP frame parser: 7-byte MBAP header (tid u16 BE, proto
 * u16 BE == 0, length u16 BE covering unit+PDU so 2..254, unit u8) then
 * length-1 PDU bytes. */
typedef struct csmb_mbap_parser {
    uint8_t  hdr[CSMB_MBAP_LEN];
    uint8_t  pdu[CSMB_MAX_PDU];
    size_t   got;          /* bytes filled in the current phase */
    int      have_header;  /* header parsed and validated */
    uint16_t plen;         /* PDU length (valid after FRAME) */
    uint16_t tid;          /* transaction id (valid after FRAME) */
    uint8_t  unit;         /* unit id (valid after FRAME) */
} csmb_mbap_parser;

void csmb_mbap_parser_init(csmb_mbap_parser *p);
void csmb_mbap_parser_reset(csmb_mbap_parser *p);
csmb_parse_ret csmb_mbap_parser_feed(csmb_mbap_parser *p,
                                     const uint8_t **in, size_t *len);

/* Modbus/RTU frame parser: unit u8 + PDU + CRC16 (low byte first).  RTU
 * has no length field, so the expected frame length is derived from the
 * function code and the role (SLAVE_MODE parses requests, otherwise
 * responses).  csmb_rtu_parser_reset() re-arms the parser after garbage
 * (driven by the transport's t3.5 idle-gap timer). */
typedef struct csmb_rtu_parser {
    int      slave_mode;
    uint8_t  buf[CSMB_MAX_PDU + 3];  /* unit + PDU + CRC (capped) */
    uint8_t  pdu[CSMB_MAX_PDU];
    size_t   got;
    size_t   expected;     /* total frame length, 0 until determined */
    uint16_t plen;         /* PDU length (valid after FRAME) */
    uint8_t  unit;         /* unit id (valid after FRAME) */
} csmb_rtu_parser;

void csmb_rtu_parser_init(csmb_rtu_parser *p, int slave_mode);
void csmb_rtu_parser_reset(csmb_rtu_parser *p);
csmb_parse_ret csmb_rtu_parser_feed(csmb_rtu_parser *p,
                                    const uint8_t **in, size_t *len);

/* Decoded response PDU (master side). */
typedef struct csmb_response {
    uint8_t  fc;               /* function code as received (0x80 = error) */
    uint8_t  exception;        /* exception code, nonzero on error response */
    uint16_t nbytes;           /* FC1-4: byte count */
    const uint8_t *data;       /* FC1-4: payload after the byte count */
    uint16_t start;            /* FC5/6/15/16: echoed address */
    uint16_t count_or_value;   /* FC5/6/15/16: echoed value/count */
} csmb_response;

/* Decode a response PDU against the fc that was requested.  Returns 0 on
 * a well-formed response (an exception is still a success: check
 * r->exception); a negative CSMB_E* on malformed length or a fc that
 * mismatches EXPECTED_FC (ignoring the 0x80 error bit). */
int csmb_decode_response(const uint8_t *pdu, size_t len,
                         uint8_t expected_fc, csmb_response *r);

/* Decoded request PDU (slave side). */
typedef struct csmb_request {
    uint8_t  fc;
    uint8_t  reg_type;   /* csmb_reg_type_t derived from fc */
    uint16_t start;
    uint16_t count;      /* 1 for FC5/FC6 */
    uint16_t value;      /* FC5: 0/1; FC6: raw */
    const uint8_t *data; /* FC15/16 payload */
    uint16_t nbytes;     /* FC15/16 byte count */
    uint8_t  exc;        /* csmb_exc_t, set on a semantic violation */
} csmb_request;

/* Decode a request PDU.  Returns a negative CSMB_E* on frame-level
 * garbage (bad length / inconsistent byte count).  Returns 0 otherwise;
 * a semantic violation (unknown fc, out-of-range count, bad coil value)
 * leaves r->exc set to the modbus exception to reply with. */
int csmb_decode_request(const uint8_t *pdu, size_t len, csmb_request *r);

/* PDU builders.  Each writes into PDU and returns the number of bytes
 * written, or a negative CSMB_E* error. */
int csmb_build_read(uint8_t *pdu, int reg_type, uint16_t start, uint16_t count);
int csmb_build_write_single(uint8_t *pdu, int reg_type, uint16_t addr,
                            uint16_t value);       /* coil: value!=0 -> 0xFF00 */
int csmb_build_write_multi(uint8_t *pdu, int reg_type, uint16_t start,
                           uint16_t count, const uint16_t *values);
int csmb_build_exception(uint8_t *pdu, uint8_t fc, uint8_t exc);
int csmb_build_read_response(uint8_t *pdu, uint8_t fc, const uint16_t *values,
                             uint16_t count, int coils);
int csmb_build_write_response(uint8_t *pdu, uint8_t fc, uint16_t start,
                              uint16_t count_or_value);

/* Framing wrappers: the PDU is already placed at the right offset
 * (buf+CSMB_MBAP_LEN for MBAP, buf+1 for RTU); these fill the header /
 * trailer and return the total frame length, or a negative error. */
int csmb_mbap_wrap(uint8_t *buf, uint16_t tid, uint8_t unit, size_t pdu_len);
int csmb_rtu_wrap(uint8_t *buf, uint8_t unit, size_t pdu_len);

/* Expand a decoded (non-exception) write request into COUNT uint16
 * register values (coils: one 0/1 per coil) written to OUT (r->count
 * entries).  Returns 0, or CSMB_EINVAL if r->fc is not a write fc. */
int csmb_request_values(const csmb_request *r, uint16_t *out);

/* ---- slave register image (csmb-image.c) ----
 *
 * Per (unit, reg_type) a sorted (by start) singly-linked list of blocks.
 * Coils/discretes store one uint16 per bit position for uniformity.
 * A safe block accepts writes but discards them and always reads 0. */

typedef struct csmb_block {
    struct csmb_block *next;
    uint16_t start;
    uint16_t count;
    uint8_t  writable;
    uint8_t  safe;
    uint16_t regs[];   /* COUNT entries */
} csmb_block;

typedef struct csmb_unit_image {
    struct csmb_unit_image *next;
    uint8_t unit;
    csmb_block *blocks[CSMB_NUM_REG_TYPES];
} csmb_unit_image;

typedef struct csmb_image {
    csmb_unit_image *units;
} csmb_image;

void csmb_image_init(csmb_image *img);
void csmb_image_free(csmb_image *img);

/* Register a data block.  WRITABLE / SAFE are 0/1.  Overlap with an
 * existing block of the same (unit, reg_type) => CSMB_EOVERLAP; count 0
 * or start+count > 65536 => CSMB_ERANGE; bad reg_type => CSMB_EBADTYPE;
 * out of memory => CSMB_ENOMEM. */
int csmb_image_register(csmb_image *img, uint8_t unit, int reg_type,
                        uint16_t start, uint16_t count, int writable, int safe);

/* Preset register values; the range must fall entirely within one
 * registered block => else CSMB_ERANGE.  Safe blocks silently discard. */
int csmb_image_set_values(csmb_image *img, uint8_t unit, int reg_type,
                          uint16_t start, uint16_t count, const uint16_t *values);

/* Ensure a unit exists in the image (blacklist bookkeeping). */
int csmb_image_touch_unit(csmb_image *img, uint8_t unit);

/* Nonzero if UNIT has at least one registered block in any reg_type. */
int csmb_image_unit_has_ranges(csmb_image *img, uint8_t unit);

/* Read COUNT registers into OUT.  Every register must lie in a
 * registered or safe block (safe reads 0).  Returns CSMB_EXC_NONE or the
 * modbus exception to reply with (CSMB_EXC_ILLEGAL_ADDRESS). */
uint8_t csmb_image_read_range(csmb_image *img, uint8_t unit, int reg_type,
                              uint16_t start, uint16_t count, uint16_t *out);

/* Apply a write of COUNT registers.  Validated first: any register that
 * is unregistered or in a read-only (non-writable, non-safe) block =>
 * CSMB_EXC_ILLEGAL_ADDRESS and nothing is applied.  Writable registers
 * are stored, safe registers discarded.  For each maximal run of applied
 * (writable, non-safe) registers, APPLY (if non-NULL) is called with the
 * run's start/count and the corresponding slice of VALUES.  Returns
 * CSMB_EXC_NONE or the modbus exception to reply with. */
typedef void (*csmb_apply_cb)(void *ctx, uint16_t start, uint16_t count,
                              const uint16_t *values);
uint8_t csmb_image_write_range(csmb_image *img, uint8_t unit, int reg_type,
                               uint16_t start, uint16_t count,
                               const uint16_t *values,
                               csmb_apply_cb apply, void *ctx);

/* ---- shared transport / connection glue (csmb-transport.c) ----
 *
 * struct lws / struct lws_vhost appear here only as incomplete-type
 * pointers; the .c files that touch them include libwebsockets.h. */

typedef enum csmb_conn_role {
    CSMB_CONN_SLAVE_CHILD = 1,  /* accepted TCP connection served by a slave */
    CSMB_CONN_MASTER_TCP  = 2,  /* master's outgoing TCP connection (later) */
    CSMB_CONN_SERIAL      = 3   /* serial / fd RTU connection (later) */
} csmb_conn_role_t;

/* One queued tx chunk with LWS_PRE headroom ahead of the payload. */
typedef struct csmb_txbuf {
    struct csmb_txbuf *next;
    size_t len;         /* payload length (excluding the LWS_PRE headroom) */
    size_t off;         /* bytes already written */
    uint8_t data[];     /* LWS_PRE headroom + LEN payload bytes */
} csmb_txbuf;

typedef struct csmb_conn {
    int role;                  /* csmb_conn_role_t */
    void *owner;               /* csmb_slave* / csmb_master*; NULL if detached */
    struct lws *wsi;
    csmb_txbuf *tx_head, *tx_tail;
    int close_requested;       /* close once the tx queue drains */
    csmb_mbap_parser mbap;     /* Modbus/TCP framing */
    struct csmb_conn *next;    /* owner's connection list */
} csmb_conn;

/* Enqueue LEN bytes (copied, with LWS_PRE headroom) and request a
 * writable callback.  Returns CSMB_OK or CSMB_ENOMEM. */
int csmb_conn_send(csmb_conn *conn, const uint8_t *frame, size_t len);

/* Flush one tx chunk (one lws_write per writable callback).  Returns -1
 * to close the connection, 0 otherwise. */
int csmb_conn_writable(csmb_conn *conn, struct lws *wsi);

/* Free the tx queue and the connection (does not unlink it from any
 * owner list; the caller does that first while the owner is alive). */
void csmb_conn_free(csmb_conn *conn);

/* The slave's listener vhost plus the heap strings lws keeps pointers to
 * (vhost never copies them). */
typedef struct csmb_listener {
    struct lws_vhost *vhost;
    char *vhost_name;
    char *role_str;
    char *proto_str;
    char *iface_str;
} csmb_listener;

/* Create a raw-skt listener vhost for TR, storing OWNER as the vhost
 * user pointer (so RAW_ADOPT can route to it).  Returns CSMB_OK,
 * CSMB_ENOMEM, or CSMB_ETRANSPORT. */
int csmb_transport_listen(csmb_listener *ln, struct lws_context *cx,
                          const struct lws_protocols *protocols,
                          const csmb_transport *tr, void *owner);
void csmb_transport_listen_close(csmb_listener *ln);

/* ---- slave hooks used by the lws dispatch (csmb-slave.c) ---- */

csmb_conn *csmb_slave_on_adopt(csmb_slave *s, struct lws *wsi);
int  csmb_slave_on_rx(csmb_slave *s, csmb_conn *conn,
                      const uint8_t *buf, size_t len);
void csmb_slave_detach_conn(csmb_slave *s, csmb_conn *conn);

/* ---- master client connect (csmb-transport.c) ----
 *
 * Start an outgoing raw-TCP connection through CLIENT_VHOST; the returned
 * csmb_conn has role CSMB_CONN_MASTER_TCP and OWNER as its owner, and its
 * wsi is filled once lws hands it back (RAW_CONNECTED / error).  Returns
 * NULL on immediate failure. */
csmb_conn *csmb_transport_connect(struct lws_context *cx,
                                  struct lws_vhost *client_vhost,
                                  const csmb_transport *tr, void *owner);

/* ---- master scheduler (csmb-sched.c) ----
 *
 * Pure logic (no lws): the span store, poll-program bunching, change
 * detection, write FIFO and per-unit bookkeeping.  Events are emitted
 * through a csmb_engine, which works without lws.  The master (csmb-
 * master.c) owns the transport, timers and pump and drives this. */

/* One subscribed span. */
typedef struct csmb_span {
    struct csmb_span *next;
    int32_t  id;
    uint8_t  unit;
    uint8_t  reg_type;
    uint16_t start;
    uint16_t count;
    uint32_t flags;        /* CSMB_SPAN_ALWAYS */
    uint16_t *values;      /* last published values (count entries), or NULL */
    int      have_values;  /* published at least once since the last clear */
    int      force;        /* refresh_span: publish the next read unconditionally */
    uint8_t  state;        /* last emitted csmb_state_t, 0 = none */
} csmb_span;

/* One request of a write op: a pre-built PDU plus verify metadata. */
typedef struct csmb_wreq {
    uint8_t  reg_type;
    uint16_t start;
    uint16_t count;
    uint16_t verify;       /* echo we expect at pdu offset 3 (count/value) */
    uint8_t  fc;
    uint8_t  pdu[CSMB_MAX_PDU];
    uint16_t plen;
} csmb_wreq;

/* A queued write op: N requests issued back-to-back. */
typedef struct csmb_write_op {
    struct csmb_write_op *next;
    int64_t  op_id;
    uint8_t  unit;
    int      nreqs;
    int      cur;          /* next request index to send */
    csmb_wreq *reqs;
} csmb_write_op;

/* Per-unit scheduler bookkeeping. */
typedef struct csmb_sunit {
    struct csmb_sunit *next;
    uint8_t  unit;
    int      enabled;
    uint32_t flags;        /* CSMB_UNIT_* */
    uint32_t request_delay_ms;
    uint32_t stale_timeout_ms;
    csmb_range *poll_seq[CSMB_NUM_REG_TYPES];   /* explicit program, or NULL */
    size_t   poll_seq_n[CSMB_NUM_REG_TYPES];
    int      online;       /* has answered since the last stale/offline */
    int      failing;      /* last request to it failed (deprioritise) */
} csmb_sunit;

/* One read request in the built poll program. */
typedef struct csmb_readstep {
    uint8_t  unit;
    uint8_t  reg_type;
    uint16_t start;
    uint16_t count;
} csmb_readstep;

typedef struct csmb_sched {
    csmb_sunit    *units;
    csmb_span     *spans;
    csmb_write_op *wq_head, *wq_tail;
    int32_t next_span_id;
    int64_t next_op_id;
    csmb_readstep *program;    /* current round's read steps */
    size_t  prog_len, prog_cap, prog_idx;
} csmb_sched;

typedef enum csmb_pick {
    CSMB_PICK_NONE  = 0,
    CSMB_PICK_READ  = 1,
    CSMB_PICK_WRITE = 2
} csmb_pick;

/* A request selected by the pump; the caller MBAP/RTU-wraps pdu[0..plen)
 * with a tid and sends it, then keeps this to interpret the response. */
typedef struct csmb_pending {
    int      kind;         /* csmb_pick */
    uint8_t  unit;
    uint8_t  fc;
    uint8_t  reg_type;
    uint16_t start;
    uint16_t count;
    uint8_t  pdu[CSMB_MAX_PDU];
    uint16_t plen;
    int64_t  op_id;        /* kind==WRITE */
    int      req_index;    /* kind==WRITE */
} csmb_pending;

void csmb_sched_init(csmb_sched *sc);
void csmb_sched_free(csmb_sched *sc);

int  csmb_sched_add_unit(csmb_sched *sc, uint8_t unit, uint32_t request_delay_ms,
                         uint32_t stale_timeout_ms, uint32_t flags);
csmb_sunit *csmb_sched_find_unit(csmb_sched *sc, uint8_t unit);

int32_t csmb_sched_subscribe(csmb_sched *sc, uint8_t unit, int reg_type,
                             uint16_t start, uint16_t count, uint32_t flags);
int  csmb_sched_unsubscribe(csmb_sched *sc, int32_t span_id);
int  csmb_sched_refresh_span(csmb_sched *sc, int32_t span_id);
int  csmb_sched_set_poll_seq(csmb_sched *sc, uint8_t unit, int reg_type,
                             const csmb_range *ranges, size_t n);
int64_t csmb_sched_enqueue_write(csmb_sched *sc, uint8_t unit,
                                 const csmb_write_spec *reqs, size_t n);

/* Bunch subscribed (start,count) ranges of REG_TYPE into read requests
 * (merging gaps up to CSMB_MAX_DUMMY_BYTES, splitting at the per-type max
 * read size).  Writes up to OUT_CAP results, returns the count needed. */
size_t csmb_bunch_ranges(int reg_type, const csmb_range *in, size_t n,
                         csmb_range *out, size_t out_cap);

/* Rebuild the read program for a fresh poll round, in failing-unit order,
 * skipping disabled units; emits one-shot CSMB_ST_UNCOVERED span-states
 * for spans a unit's explicit poll_seq does not cover. */
void csmb_sched_start_round(csmb_engine *e, csmb_sched *sc);

/* Select the next request: a write op step (preempting reads) if any is
 * pending, otherwise the next read of the current round.  Returns a
 * csmb_pick and fills *p when non-NONE. */
int  csmb_sched_pick(csmb_sched *sc, csmb_pending *p);

/* Apply a well-formed response to the pending request: marks the unit
 * online (republishing on recovery), advances/completes write ops, and
 * runs change detection emitting SPAN_UPDATE / SPAN_STATE / WRITE_DONE /
 * UNIT_STATE events on E. */
void csmb_sched_on_response(csmb_engine *e, csmb_sched *sc,
                            const csmb_pending *p, const csmb_response *r);

/* A pending request got no usable answer (response timeout / connection
 * error).  Reads mark the unit failing (and emit a one-shot LOG); a write
 * op fails with WR_STATUS. */
void csmb_sched_on_request_failed(csmb_engine *e, csmb_sched *sc,
                                  const csmb_pending *p, int wr_status);

/* The per-unit stale timer fired: the unit's spans go STALE and it goes
 * offline (values cleared so recovery republishes). */
void csmb_sched_mark_unit_stale(csmb_engine *e, csmb_sched *sc, uint8_t unit);

/* Enable/disable polling of a unit; disabling fails its queued writes
 * with CSMB_WR_UNIT_DISABLED and staled its spans, enabling clears their
 * values so everything republishes.  Returns CSMB_OK / CSMB_ENOUNIT. */
int  csmb_sched_set_unit_enabled(csmb_engine *e, csmb_sched *sc,
                                 uint8_t unit, int enabled);

/* Transport went down: all spans go OFFLINE with values cleared, all
 * units offline, queued writes fail with WR_CONN_FAILED. */
void csmb_sched_connection_down(csmb_engine *e, csmb_sched *sc);

/* Transport (re)connected: clear every span's published values so the
 * next reads republish; drop the stale poll program. */
void csmb_sched_connection_up(csmb_sched *sc);

/* ---- master hooks used by the lws dispatch (csmb-master.c) ---- */

void csmb_master_on_connected(csmb_master *m, struct lws *wsi);
void csmb_master_on_connect_error(csmb_master *m);
int  csmb_master_on_rx(csmb_master *m, const uint8_t *buf, size_t len);
void csmb_master_on_closed(csmb_master *m);

#endif /* CSMB_PRIVATE_H_INCLUDED */
