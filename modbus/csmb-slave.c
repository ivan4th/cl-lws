/* Slave engine: TCP listener, request handling against the register
 * image, slave-write events.  Serial / fd transports are a later stage.
 *
 * A csmb_slave begins with a csmb_engine header (csmb_events_get/done
 * cast through it).  Accepted connections hang off a singly-linked list;
 * csmb_slave_destroy detaches them (owner -> NULL) and asks lws to close
 * them, so an in-flight connection outlives the slave safely: further RX
 * on a detached conn is dropped, and RAW_CLOSE frees the conn without
 * touching the freed slave. */

#include <libwebsockets.h>
#include "csmb.h"
#include "csmb-private.h"

struct csmb_slave {
    csmb_engine engine;              /* MUST be first */
    csmb_image image;
    csmb_listener listener;
    int has_listener;
    uint8_t blacklist[256];          /* per-unit csmb_bl_* mode */
    csmb_conn *conns;                /* accepted-connection list */
};

/* ---- lifecycle ---- */

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
        /* later stage: RTU slave over serial / fd */
        goto fail;
    default:
        goto fail;
    }
    return s;

fail:
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

    /* Detach every accepted connection before tearing down the vhost:
     * their RAW_CLOSE (which the vhost destroy triggers) then frees them
     * without dereferencing the freed slave. */
    for (c = s->conns; c; c = c->next) {
        c->owner = NULL;
        if (c->wsi)
            lws_set_timeout(c->wsi, PENDING_TIMEOUT_HTTP_CONTENT, LWS_TO_KILL_ASYNC);
    }
    s->conns = NULL;

    if (s->has_listener)
        csmb_transport_listen_close(&s->listener);
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
    csmb_mbap_parser_init(&conn->mbap);
    conn->next = s->conns;
    s->conns = conn;
    lws_set_opaque_user_data(wsi, conn);
    return conn;
}

void csmb_slave_detach_conn(csmb_slave *s, csmb_conn *conn)
{
    csmb_conn **pp = &s->conns;

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

/* MBAP-wrap PDU with the request's tid/unit and enqueue it. */
static int slave_send(csmb_conn *conn, uint16_t tid, uint8_t unit,
                      const uint8_t *pdu, int pdu_len)
{
    uint8_t frame[CSMB_MBAP_LEN + CSMB_MAX_PDU];
    int flen;

    if (pdu_len < 0)
        return -1;
    memcpy(frame + CSMB_MBAP_LEN, pdu, (size_t)pdu_len);
    flen = csmb_mbap_wrap(frame, tid, unit, (size_t)pdu_len);
    if (flen < 0)
        return -1;
    csmb_conn_send(conn, frame, (size_t)flen);
    return 0;
}

/* Handle one decoded request frame.  Returns 0, or -1 to close the
 * connection (hard blacklist / frame garbage). */
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

    if (req.exc) {
        rlen = csmb_build_exception(resp, req.fc, req.exc);
        return slave_send(conn, tid, unit, resp, rlen);
    }

    switch (req.fc) {
    case CSMB_FC_READ_COILS:
    case CSMB_FC_READ_DISCRETE:
    case CSMB_FC_READ_HOLDING:
    case CSMB_FC_READ_INPUT: {
        uint16_t vals[CSMB_MAX_READ_FLAGS];
        uint8_t exc = csmb_image_read_range(&s->image, unit, req.reg_type,
                                            req.start, req.count, vals);
        if (exc) {
            rlen = csmb_build_exception(resp, req.fc, exc);
        } else {
            int coils = (req.reg_type == CSMB_COIL ||
                         req.reg_type == CSMB_DISCRETE);
            rlen = csmb_build_read_response(resp, req.fc, vals, req.count, coils);
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

        csmb_request_values(&req, vals);
        ctx.s = s;
        ctx.unit = unit;
        ctx.reg_type = req.reg_type;
        exc = csmb_image_write_range(&s->image, unit, req.reg_type,
                                     req.start, req.count, vals,
                                     slave_emit_write, &ctx);
        if (exc) {
            rlen = csmb_build_exception(resp, req.fc, exc);
        } else {
            /* echo the request: FC5 coil state, FC6 value, FC15/16 count */
            if (req.fc == CSMB_FC_WRITE_SINGLE_COIL)
                echo = req.value ? 0xFF00 : 0x0000;
            else if (req.fc == CSMB_FC_WRITE_SINGLE_REG)
                echo = req.value;
            else
                echo = req.count;
            rlen = csmb_build_write_response(resp, req.fc, req.start, echo);
        }
        return slave_send(conn, tid, unit, resp, rlen);
    }

    default:
        rlen = csmb_build_exception(resp, req.fc, CSMB_EXC_ILLEGAL_FUNCTION);
        return slave_send(conn, tid, unit, resp, rlen);
    }
}

int csmb_slave_on_rx(csmb_slave *s, csmb_conn *conn,
                     const uint8_t *buf, size_t len)
{
    const uint8_t *in = buf;

    while (len > 0) {
        csmb_parse_ret pr = csmb_mbap_parser_feed(&conn->mbap, &in, &len);

        if (pr == CSMB_PR_NEED_MORE)
            break;
        if (pr == CSMB_PR_BAD)
            return -1;   /* unframeable: close the connection */
        /* CSMB_PR_FRAME */
        if (slave_handle_frame(s, conn, conn->mbap.tid, conn->mbap.unit,
                               conn->mbap.pdu, conn->mbap.plen) < 0)
            return -1;
        csmb_mbap_parser_reset(&conn->mbap);
    }
    return 0;
}
