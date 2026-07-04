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

#endif /* CSMB_PRIVATE_H_INCLUDED */
