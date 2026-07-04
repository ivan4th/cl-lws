/* Slave engine: TCP listener and/or a serial RTU bus, request handling
 * against the register image, slave-write events.
 *
 * A csmb_slave begins with a csmb_engine header (csmb_events_get/done
 * cast through it).  Connections hang off a singly-linked list;
 * csmb_slave_destroy detaches them (owner -> NULL) and asks lws to close
 * them, so an in-flight connection outlives the slave safely: further RX
 * on a detached conn is dropped, and RAW_CLOSE frees the conn without
 * touching the freed slave.
 *
 * TCP is a multi-connection gateway (unknown/soft-blacklisted units get a
 * gateway-target exception, hard-blacklisted units drop the socket).  A
 * serial RTU bus is multi-drop: the slave answers only the units it
 * serves and stays silent otherwise (no gateway exceptions, never drop
 * the bus); unit 0 is a broadcast (writes applied, no reply). */

#include <libwebsockets.h>
#include <unistd.h>
#include "csmb.h"
#include "csmb-private.h"

struct csmb_slave {
    csmb_engine engine;              /* MUST be first */
    csmb_image image;
    csmb_listener listener;          /* TCP listener vhost */
    int has_listener;
    csmb_listener serial_vhost;      /* non-listening vhost for fd adoption */
    int has_serial;
    csmb_conn *serial_conn;          /* the serial RTU bus conn, or NULL */
    struct lws_sorted_usec_list t35_sul;   /* RTU idle-gap resync timer */
    uint32_t t35_us;
    uint8_t blacklist[256];          /* per-unit csmb_bl_* mode */
    csmb_conn *conns;                /* connection list */
};

/* ---- lifecycle ---- */

static int slave_open_serial(csmb_slave *s, struct lws_context *cx,
                             const struct lws_protocols *protocols,
                             const csmb_transport *tr)
{
    int fd;

    if (!protocols)
        return CSMB_ETRANSPORT;   /* fd adoption needs a protocols array */
    fd = (tr->kind == CSMB_TR_FD) ? tr->fd : csmb_serial_open(tr);
    if (fd < 0)
        return CSMB_ETRANSPORT;
    if (csmb_transport_serial_vhost(&s->serial_vhost, cx, protocols, s) != CSMB_OK) {
        close(fd);
        return CSMB_ETRANSPORT;
    }
    s->has_serial = 1;
    s->serial_conn = csmb_transport_adopt_fd(s->serial_vhost.vhost, fd,
                                             CSMB_CONN_SLAVE_SERIAL, s, 1);
    if (!s->serial_conn) {
        close(fd);
        return CSMB_ETRANSPORT;
    }
    s->serial_conn->next = s->conns;
    s->conns = s->serial_conn;
    s->t35_us = csmb_serial_t35_us(tr);
    return CSMB_OK;
}

csmb_slave *csmb_slave_create(struct lws_context *cx,
                              const struct lws_protocols *protocols,
                              const csmb_transport *tr,
                              csmb_notify_cb notify, void *opaque)
{
    csmb_slave *s = calloc(1, sizeof(*s));

    if (!s)
        return NULL;
    csmb_engine_init(&s->engine, cx, notify, opaque);
    csmb_image_init(&s->image);

    switch (tr->kind) {
    case CSMB_TR_TCP:
        if (csmb_transport_listen(&s->listener, cx, protocols, tr, s) != CSMB_OK)
            goto fail;
        s->has_listener = 1;
        break;
    case CSMB_TR_SERIAL:
    case CSMB_TR_FD:
        if (slave_open_serial(s, cx, protocols, tr) != CSMB_OK)
            goto fail;
        break;
    default:
        goto fail;
    }
    return s;

fail:
    if (s->has_serial)
        csmb_transport_listen_close(&s->serial_vhost);
    csmb_image_free(&s->image);
    csmb_engine_free(&s->engine);
    free(s);
    return NULL;
}

void csmb_slave_destroy(csmb_slave *s)
{
    csmb_conn *c;

    if (!s)
        return;
    s->engine.destroyed = 1;
    lws_sul_cancel(&s->t35_sul);

    /* Detach every connection before tearing down the vhosts: their
     * RAW_CLOSE(_FILE) (which vhost destroy triggers) then frees them
     * without dereferencing the freed slave. */
    for (c = s->conns; c; c = c->next) {
        c->owner = NULL;
        if (c->wsi)
            lws_set_timeout(c->wsi, PENDING_TIMEOUT_HTTP_CONTENT, LWS_TO_KILL_ASYNC);
    }
    s->conns = NULL;
    s->serial_conn = NULL;

    if (s->has_listener)
        csmb_transport_listen_close(&s->listener);
    if (s->has_serial)
        csmb_transport_listen_close(&s->serial_vhost);
    csmb_image_free(&s->image);
    csmb_engine_free(&s->engine);
    free(s);
}

int csmb_slave_listen_port(csmb_slave *s)
{
    if (!s->has_listener)
        return -1;
    return lws_get_vhost_listen_port(s->listener.vhost);
}

/* ---- image configuration API ---- */

int csmb_slave_register_range(csmb_slave *s, uint8_t unit, int reg_type,
                              uint16_t start, uint16_t count, int writable)
{
    return csmb_image_register(&s->image, unit, reg_type, start, count,
                               writable, 0);
}

int csmb_slave_set_safe_range(csmb_slave *s, uint8_t unit, int reg_type,
                              uint16_t start, uint16_t count)
{
    return csmb_image_register(&s->image, unit, reg_type, start, count, 0, 1);
}

int csmb_slave_set_values(csmb_slave *s, uint8_t unit, int reg_type,
                          uint16_t start, uint16_t count,
                          const uint16_t *values)
{
    return csmb_image_set_values(&s->image, unit, reg_type, start, count, values);
}

int csmb_slave_blacklist(csmb_slave *s, uint8_t unit, int mode)
{
    if (mode < CSMB_BL_NONE || mode > CSMB_BL_HARD)
        return CSMB_EINVAL;
    s->blacklist[unit] = (uint8_t)mode;
    return csmb_image_touch_unit(&s->image, unit);
}

/* ---- connection bookkeeping (called from the lws dispatch) ---- */

csmb_conn *csmb_slave_on_adopt(csmb_slave *s, struct lws *wsi)
{
    csmb_conn *conn = calloc(1, sizeof(*conn));

    if (!conn)
        return NULL;
    conn->role = CSMB_CONN_SLAVE_CHILD;
    conn->owner = s;
    conn->wsi = wsi;
    conn->fd = -1;
    csmb_mbap_parser_init(&conn->mbap);
    conn->next = s->conns;
    s->conns = conn;
    lws_set_opaque_user_data(wsi, conn);
    return conn;
}

void csmb_slave_detach_conn(csmb_slave *s, csmb_conn *conn)
{
    csmb_conn **pp = &s->conns;

    if (conn == s->serial_conn) {
        s->serial_conn = NULL;
        lws_sul_cancel(&s->t35_sul);
    }
    while (*pp) {
        if (*pp == conn) {
            *pp = conn->next;
            return;
        }
        pp = &(*pp)->next;
    }
}

/* ---- request handling ---- */

struct slave_emit_ctx {
    csmb_slave *s;
    uint8_t unit;
    uint8_t reg_type;
};

static void slave_emit_write(void *vctx, uint16_t start, uint16_t count,
                             const uint16_t *values)
{
    struct slave_emit_ctx *ctx = vctx;
    CSMB_ZERO_EVENT(ev);

    ev.type = CSMB_EV_SLAVE_WRITE;
    ev.unit = ctx->unit;
    ev.reg_type = ctx->reg_type;
    ev.start = start;
    csmb_event_add((csmb_engine *)ctx->s, &ev, values, count);
}

/* Wrap PDU for CONN's framing (MBAP with TID on TCP, RTU on serial) and
 * enqueue it. */
static int slave_send(csmb_conn *conn, uint16_t tid, uint8_t unit,
                      const uint8_t *pdu, int pdu_len)
{
    if (pdu_len < 0)
        return -1;
    if (csmb_conn_is_serial(conn)) {
        uint8_t frame[1 + CSMB_MAX_PDU + 2];
        int flen = (memcpy(frame + 1, pdu, (size_t)pdu_len),
                    csmb_rtu_wrap(frame, unit, (size_t)pdu_len));

        if (flen < 0)
            return -1;
        csmb_conn_send(conn, frame, (size_t)flen);
    } else {
        uint8_t frame[CSMB_MBAP_LEN + CSMB_MAX_PDU];
        int flen = (memcpy(frame + CSMB_MBAP_LEN, pdu, (size_t)pdu_len),
                    csmb_mbap_wrap(frame, tid, unit, (size_t)pdu_len));

        if (flen < 0)
            return -1;
        csmb_conn_send(conn, frame, (size_t)flen);
    }
    return 0;
}

/* Apply/answer a decoded, served request against UNIT's image.  When
 * RESPOND is 0 (serial broadcast) writes are applied but nothing is sent
 * and reads are ignored.  Returns 0, or -1 to close the connection. */
static int slave_answer(csmb_slave *s, csmb_conn *conn, uint16_t tid,
                        uint8_t unit, const csmb_request *req, int respond)
{
    uint8_t resp[CSMB_MAX_PDU];
    int rlen;

    if (req->exc) {
        if (!respond)
            return 0;
        rlen = csmb_build_exception(resp, req->fc, req->exc);
        return slave_send(conn, tid, unit, resp, rlen);
    }

    switch (req->fc) {
    case CSMB_FC_READ_COILS:
    case CSMB_FC_READ_DISCRETE:
    case CSMB_FC_READ_HOLDING:
    case CSMB_FC_READ_INPUT: {
        uint16_t vals[CSMB_MAX_READ_FLAGS];
        uint8_t exc;

        if (!respond)
            return 0;   /* broadcast reads are ignored */
        exc = csmb_image_read_range(&s->image, unit, req->reg_type,
                                    req->start, req->count, vals);
        if (exc) {
            rlen = csmb_build_exception(resp, req->fc, exc);
        } else {
            int coils = (req->reg_type == CSMB_COIL ||
                         req->reg_type == CSMB_DISCRETE);
            rlen = csmb_build_read_response(resp, req->fc, vals, req->count, coils);
        }
        return slave_send(conn, tid, unit, resp, rlen);
    }

    case CSMB_FC_WRITE_SINGLE_COIL:
    case CSMB_FC_WRITE_SINGLE_REG:
    case CSMB_FC_WRITE_MULTI_COILS:
    case CSMB_FC_WRITE_MULTI_REGS: {
        uint16_t vals[CSMB_MAX_WRITE_COILS];
        struct slave_emit_ctx ctx;
        uint8_t exc;
        uint16_t echo;

        csmb_request_values(req, vals);
        ctx.s = s;
        ctx.unit = unit;
        ctx.reg_type = req->reg_type;
        exc = csmb_image_write_range(&s->image, unit, req->reg_type,
                                     req->start, req->count, vals,
                                     slave_emit_write, &ctx);
        if (!respond)
            return 0;
        if (exc) {
            rlen = csmb_build_exception(resp, req->fc, exc);
        } else {
            if (req->fc == CSMB_FC_WRITE_SINGLE_COIL)
                echo = req->value ? 0xFF00 : 0x0000;
            else if (req->fc == CSMB_FC_WRITE_SINGLE_REG)
                echo = req->value;
            else
                echo = req->count;
            rlen = csmb_build_write_response(resp, req->fc, req->start, echo);
        }
        return slave_send(conn, tid, unit, resp, rlen);
    }

    default:
        if (!respond)
            return 0;
        rlen = csmb_build_exception(resp, req->fc, CSMB_EXC_ILLEGAL_FUNCTION);
        return slave_send(conn, tid, unit, resp, rlen);
    }
}

/* TCP: gateway semantics.  Returns 0, or -1 to close the socket. */
static int slave_handle_frame(csmb_slave *s, csmb_conn *conn, uint16_t tid,
                              uint8_t unit, const uint8_t *pdu, uint16_t plen)
{
    uint8_t mode = s->blacklist[unit];
    uint8_t resp[CSMB_MAX_PDU];
    csmb_request req;
    int rlen;

    if (mode == CSMB_BL_HARD)
        return -1;
    if (csmb_decode_request(pdu, plen, &req) < 0)
        return -1;   /* frame-level garbage: resync is not defined for TCP */
    if (mode == CSMB_BL_SOFT || !csmb_image_unit_has_ranges(&s->image, unit)) {
        rlen = csmb_build_exception(resp, req.fc, CSMB_EXC_GW_TARGET);
        return slave_send(conn, tid, unit, resp, rlen);
    }
    return slave_answer(s, conn, tid, unit, &req, 1);
}

/* Serial RTU: multi-drop semantics.  Answers only served units, stays
 * silent otherwise; unit 0 is a broadcast (writes applied, no reply). */
static void slave_handle_frame_serial(csmb_slave *s, csmb_conn *conn,
                                      uint8_t unit, const uint8_t *pdu,
                                      uint16_t plen)
{
    csmb_request req;

    if (csmb_decode_request(pdu, plen, &req) < 0)
        return;                       /* garbage: silent, resync on t3.5 */
    if (unit == 0) {
        slave_answer(s, conn, 0, 0, &req, 0);   /* broadcast: apply, no reply */
        return;
    }
    if (s->blacklist[unit] != CSMB_BL_NONE)
        return;                       /* blacklisted: silent (never drop the bus) */
    if (!csmb_image_unit_has_ranges(&s->image, unit))
        return;                       /* not our unit: silent */
    slave_answer(s, conn, 0, unit, &req, 1);
}

static void slave_t35_cb(struct lws_sorted_usec_list *sul)
{
    csmb_slave *s = lws_container_of(sul, csmb_slave, t35_sul);

    if (s->serial_conn)
        csmb_rtu_parser_reset(&s->serial_conn->rtu);
}

int csmb_slave_on_rx(csmb_slave *s, csmb_conn *conn,
                     const uint8_t *buf, size_t len)
{
    const uint8_t *in = buf;

    if (conn->role == CSMB_CONN_SLAVE_SERIAL) {
        /* re-arm the t3.5 idle-gap resync timer on every RX chunk */
        lws_sul_schedule(s->engine.cx, 0, &s->t35_sul, slave_t35_cb,
                         (lws_usec_t)s->t35_us);
        while (len > 0) {
            csmb_parse_ret pr = csmb_rtu_parser_feed(&conn->rtu, &in, &len);

            if (pr == CSMB_PR_NEED_MORE)
                break;
            if (pr == CSMB_PR_BAD) {
                csmb_rtu_parser_reset(&conn->rtu);   /* resync */
                continue;
            }
            slave_handle_frame_serial(s, conn, conn->rtu.unit, conn->rtu.pdu,
                                      conn->rtu.plen);
            csmb_rtu_parser_reset(&conn->rtu);
        }
        return 0;   /* never close the shared bus */
    }

    while (len > 0) {
        csmb_parse_ret pr = csmb_mbap_parser_feed(&conn->mbap, &in, &len);

        if (pr == CSMB_PR_NEED_MORE)
            break;
        if (pr == CSMB_PR_BAD)
            return -1;   /* unframeable: close the connection */
        if (slave_handle_frame(s, conn, conn->mbap.tid, conn->mbap.unit,
                               conn->mbap.pdu, conn->mbap.plen) < 0)
            return -1;
        csmb_mbap_parser_reset(&conn->mbap);
    }
    return 0;
}
