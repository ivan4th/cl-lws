/* Master engine: one outgoing connection (TCP, serial or fd) with
 * reconnect/backoff, a heartbeat-driven poll pump over the scheduler,
 * response timeouts, per-unit staleness timers and event emission.
 *
 * A csmb_master begins with a csmb_engine header (csmb_events_get/done
 * cast through it).  All logic (span store, bunching, change detection,
 * write FIFO) lives in the pure csmb_sched; this file owns the lws
 * transport and timers and drives the scheduler.  TCP frames carry an
 * MBAP header (responses matched by transaction id); serial/fd frames are
 * RTU (no tid, responses matched by unit + function code). */

#include <libwebsockets.h>
#include <unistd.h>
#include "csmb-private.h"

#define CSMB_BACKOFF_MIN_MS 500
#define CSMB_BACKOFF_MAX_MS 8000
#define CSMB_DEADPEER_TIMEOUTS 5   /* consecutive timeouts -> drop the conn */

typedef struct csmb_stale_timer {
    struct lws_sorted_usec_list sul;
    csmb_master *m;
    uint8_t unit;
    int armed;
} csmb_stale_timer;

struct csmb_master {
    csmb_engine engine;              /* MUST be first */
    struct lws_vhost *client_vhost;
    csmb_transport tr;
    char *host;
    csmb_sched sched;

    csmb_conn *conn;                 /* the single outgoing connection */
    int conn_state;                  /* csmb_conn_state_t */

    struct lws_sorted_usec_list reconnect_sul;   /* also the initial connect */
    struct lws_sorted_usec_list heartbeat_sul;
    struct lws_sorted_usec_list resp_sul;
    struct lws_sorted_usec_list pace_sul;
    struct lws_sorted_usec_list t35_sul;         /* serial RTU idle-gap resync */
    csmb_stale_timer stale[256];

    uint32_t backoff_ms;
    uint32_t heartbeat_ms;
    uint32_t response_timeout_ms;
    uint32_t t35_us;                 /* serial inter-frame gap */

    lws_usec_t pace_until;

    int in_flight;
    uint16_t cur_tid;
    uint16_t next_tid;
    csmb_pending pending;
    int round_active;
    int timeout_streak;              /* consecutive response timeouts */
};

/* ---- forward decls ---- */

static void master_pump(csmb_master *m);
static void start_connect(csmb_master *m);
static void schedule_reconnect(csmb_master *m);
static void master_go_down(csmb_master *m, uint8_t cerr);
static void master_go_online(csmb_master *m);
static void master_force_close(csmb_master *m, uint8_t cerr);
static void resp_cb(struct lws_sorted_usec_list *sul);
static void pace_cb(struct lws_sorted_usec_list *sul);

/* ---- event helpers ---- */

static void emit_conn_state(csmb_master *m, uint8_t state, uint8_t cerr)
{
    CSMB_ZERO_EVENT(ev);
    ev.type = CSMB_EV_CONN_STATE;
    ev.state = state;
    ev.exception = cerr;
    csmb_event_add(&m->engine, &ev, NULL, 0);
}

/* ---- per-unit stale timers ---- */

static void stale_cb(struct lws_sorted_usec_list *sul)
{
    csmb_stale_timer *st = lws_container_of(sul, csmb_stale_timer, sul);
    csmb_master *m = st->m;

    st->armed = 0;
    csmb_sched_mark_unit_stale(&m->engine, &m->sched, st->unit);
    csmb_event_flush(&m->engine);
}

static void reset_stale_timer(csmb_master *m, uint8_t unit)
{
    csmb_sunit *u = csmb_sched_find_unit(&m->sched, unit);
    uint32_t to = u ? u->stale_timeout_ms : 20000;

    m->stale[unit].armed = 1;
    lws_sul_schedule(m->engine.cx, 0, &m->stale[unit].sul, stale_cb,
                     (lws_usec_t)to * 1000);
}

static void arm_stale_if_needed(csmb_master *m, uint8_t unit)
{
    if (!m->stale[unit].armed)
        reset_stale_timer(m, unit);
}

static void cancel_all_stale(csmb_master *m)
{
    int i;

    for (i = 0; i < 256; i++)
        if (m->stale[i].armed) {
            lws_sul_cancel(&m->stale[i].sul);
            m->stale[i].armed = 0;
        }
}

static void master_t35_cb(struct lws_sorted_usec_list *sul)
{
    csmb_master *m = lws_container_of(sul, csmb_master, t35_sul);

    if (m->conn && csmb_conn_is_serial(m->conn))
        csmb_rtu_parser_reset(&m->conn->rtu);
}

/* ---- request pump ---- */

static void master_pump(csmb_master *m)
{
    lws_usec_t now;
    int k, serial;
    uint32_t delay_us;

    if (m->engine.destroyed || m->conn_state != CSMB_CONN_ONLINE ||
        m->in_flight || !m->conn)
        return;

    now = lws_now_usecs();
    if (m->pace_until > now) {
        lws_sul_schedule(m->engine.cx, 0, &m->pace_sul, pace_cb,
                         (lws_usec_t)(m->pace_until - now));
        return;
    }

    k = csmb_sched_pick(&m->sched, &m->pending);
    if (k == CSMB_PICK_NONE) {
        m->round_active = 0;
        return;
    }

    serial = csmb_conn_is_serial(m->conn);
    if (serial) {
        uint8_t frame[1 + CSMB_MAX_PDU + 2];
        int flen;

        memcpy(frame + 1, m->pending.pdu, m->pending.plen);
        flen = csmb_rtu_wrap(frame, m->pending.unit, m->pending.plen);
        if (flen < 0)
            return;
        m->in_flight = 1;
        csmb_conn_send(m->conn, frame, (size_t)flen);
    } else {
        uint8_t frame[CSMB_MBAP_LEN + CSMB_MAX_PDU];
        int flen;
        uint16_t tid = m->next_tid++;

        memcpy(frame + CSMB_MBAP_LEN, m->pending.pdu, m->pending.plen);
        flen = csmb_mbap_wrap(frame, tid, m->pending.unit, m->pending.plen);
        if (flen < 0)
            return;
        m->cur_tid = tid;
        m->in_flight = 1;
        csmb_conn_send(m->conn, frame, (size_t)flen);
    }

    arm_stale_if_needed(m, m->pending.unit);
    lws_sul_schedule(m->engine.cx, 0, &m->resp_sul, resp_cb,
                     (lws_usec_t)m->response_timeout_ms * 1000);

    {
        csmb_sunit *u = csmb_sched_find_unit(&m->sched, m->pending.unit);

        delay_us = (u ? u->request_delay_ms : 0) * 1000;
        if (serial && m->t35_us > delay_us)
            delay_us = m->t35_us;   /* keep a t3.5 gap between frames */
    }
    m->pace_until = lws_now_usecs() + (lws_usec_t)delay_us;
}

static void master_kick(csmb_master *m)
{
    if (m->conn_state == CSMB_CONN_ONLINE && !m->in_flight && !m->round_active) {
        m->round_active = 1;
        csmb_sched_start_round(&m->engine, &m->sched);
        master_pump(m);
    }
}

/* ---- timer callbacks ---- */

static void resp_cb(struct lws_sorted_usec_list *sul)
{
    csmb_master *m = lws_container_of(sul, csmb_master, resp_sul);

    if (!m->in_flight)
        return;
    m->in_flight = 0;
    m->timeout_streak++;
    csmb_sched_on_request_failed(&m->engine, &m->sched, &m->pending,
                                 m->pending.kind == CSMB_PICK_WRITE
                                 ? CSMB_WR_TIMEOUT : 0, lws_now_usecs());
    /* dead peer: no valid frame at all across several timeouts -> drop */
    if (m->timeout_streak >= CSMB_DEADPEER_TIMEOUTS)
        master_force_close(m, CSMB_CERR_TIMEOUT);
    else
        master_pump(m);
    csmb_event_flush(&m->engine);
}

static void pace_cb(struct lws_sorted_usec_list *sul)
{
    csmb_master *m = lws_container_of(sul, csmb_master, pace_sul);

    master_pump(m);
    csmb_event_flush(&m->engine);
}

static void heartbeat_cb(struct lws_sorted_usec_list *sul)
{
    csmb_master *m = lws_container_of(sul, csmb_master, heartbeat_sul);

    lws_sul_schedule(m->engine.cx, 0, &m->heartbeat_sul, heartbeat_cb,
                     (lws_usec_t)m->heartbeat_ms * 1000);
    if (m->conn_state == CSMB_CONN_ONLINE && !m->in_flight && !m->round_active) {
        m->round_active = 1;
        csmb_sched_start_round(&m->engine, &m->sched);
        master_pump(m);
    }
    csmb_event_flush(&m->engine);
}

static void reconnect_cb(struct lws_sorted_usec_list *sul)
{
    csmb_master *m = lws_container_of(sul, csmb_master, reconnect_sul);

    start_connect(m);
    csmb_event_flush(&m->engine);
}

/* ---- connection FSM ---- */

static void start_connect(csmb_master *m)
{
    if (m->engine.destroyed)
        return;
    m->conn_state = CSMB_CONN_CONNECTING;
    emit_conn_state(m, CSMB_CONN_CONNECTING, CSMB_CERR_NONE);

    if (m->tr.kind == CSMB_TR_TCP) {
        m->conn = csmb_transport_connect(m->engine.cx, m->client_vhost, &m->tr, m);
        if (!m->conn)
            master_go_down(m, CSMB_CERR_CONNECT_FAILED);
        return;   /* else wait for RAW_CONNECTED / CLIENT_CONNECTION_ERROR */
    }

    /* serial / fd: adopt the descriptor, then go online at once (there is
     * no RAW_CONNECTED for raw files) */
    {
        int fd = (m->tr.kind == CSMB_TR_FD) ? m->tr.fd : csmb_serial_open(&m->tr);

        if (fd < 0) {
            master_go_down(m, CSMB_CERR_CONNECT_FAILED);
            return;
        }
        m->conn = csmb_transport_adopt_fd(m->client_vhost, fd,
                                          CSMB_CONN_MASTER_SERIAL, m, 0);
        if (!m->conn) {
            close(fd);
            master_go_down(m, CSMB_CERR_CONNECT_FAILED);
            return;
        }
        m->t35_us = csmb_serial_t35_us(&m->tr);
        master_go_online(m);
    }
}

static void schedule_reconnect(csmb_master *m)
{
    if (m->engine.destroyed)
        return;
    if (m->tr.kind == CSMB_TR_FD)
        return;   /* a handed-over fd cannot be reopened: stay offline */
    if (m->backoff_ms == 0)
        m->backoff_ms = CSMB_BACKOFF_MIN_MS;
    else
        m->backoff_ms = m->backoff_ms >= CSMB_BACKOFF_MAX_MS
                        ? CSMB_BACKOFF_MAX_MS : m->backoff_ms * 2;
    lws_sul_schedule(m->engine.cx, 0, &m->reconnect_sul, reconnect_cb,
                     (lws_usec_t)m->backoff_ms * 1000);
}

static void master_go_down(csmb_master *m, uint8_t cerr)
{
    if (m->conn_state == CSMB_CONN_OFFLINE)
        return;
    m->conn_state = CSMB_CONN_OFFLINE;
    emit_conn_state(m, CSMB_CONN_OFFLINE, cerr);
    csmb_sched_connection_down(&m->engine, &m->sched);
    m->in_flight = 0;
    m->round_active = 0;
    m->pace_until = 0;
    m->timeout_streak = 0;
    lws_sul_cancel(&m->resp_sul);
    lws_sul_cancel(&m->pace_sul);
    lws_sul_cancel(&m->heartbeat_sul);
    lws_sul_cancel(&m->t35_sul);
    cancel_all_stale(m);
    schedule_reconnect(m);
}

static void master_go_online(csmb_master *m)
{
    m->conn_state = CSMB_CONN_ONLINE;
    emit_conn_state(m, CSMB_CONN_ONLINE, CSMB_CERR_NONE);
    m->backoff_ms = 0;
    m->timeout_streak = 0;
    csmb_sched_connection_up(&m->sched);
    lws_sul_schedule(m->engine.cx, 0, &m->heartbeat_sul, heartbeat_cb,
                     (lws_usec_t)m->heartbeat_ms * 1000);
    m->round_active = 1;
    csmb_sched_start_round(&m->engine, &m->sched);
    master_pump(m);
}

/* Tear down the current connection ourselves (dead peer) and reconnect. */
static void master_force_close(csmb_master *m, uint8_t cerr)
{
    struct lws *wsi = m->conn ? m->conn->wsi : NULL;

    if (m->conn) {
        m->conn->owner = NULL;   /* detach: RAW_CLOSE just frees the conn */
        m->conn = NULL;
    }
    master_go_down(m, cerr);
    if (wsi)
        lws_set_timeout(wsi, PENDING_TIMEOUT_HTTP_CONTENT, LWS_TO_KILL_ASYNC);
}

void csmb_master_on_connected(csmb_master *m, struct lws *wsi)
{
    (void)wsi;
    master_go_online(m);
}

void csmb_master_on_connect_error(csmb_master *m)
{
    m->conn = NULL;   /* the dispatch frees the conn object */
    master_go_down(m, CSMB_CERR_CONNECT_FAILED);
}

void csmb_master_on_closed(csmb_master *m)
{
    m->conn = NULL;
    master_go_down(m, CSMB_CERR_CLOSED);
}

int csmb_master_on_rx(csmb_master *m, const uint8_t *buf, size_t len)
{
    const uint8_t *in = buf;
    int processed = 0, serial;

    if (!m->conn)
        return 0;
    serial = csmb_conn_is_serial(m->conn);
    if (serial)
        lws_sul_schedule(m->engine.cx, 0, &m->t35_sul, master_t35_cb,
                         (lws_usec_t)m->t35_us);

    while (len > 0) {
        csmb_parse_ret pr;
        const uint8_t *r_pdu;
        uint16_t r_plen;
        uint8_t r_unit;
        int match;

        if (serial) {
            pr = csmb_rtu_parser_feed(&m->conn->rtu, &in, &len);
            if (pr == CSMB_PR_NEED_MORE)
                break;
            if (pr == CSMB_PR_BAD) {
                csmb_rtu_parser_reset(&m->conn->rtu);   /* resync on t3.5 */
                continue;
            }
            r_pdu = m->conn->rtu.pdu;
            r_plen = m->conn->rtu.plen;
            r_unit = m->conn->rtu.unit;
            /* RTU has no tid: match on unit + function code */
            match = m->in_flight && r_unit == m->pending.unit &&
                    r_plen > 0 && (r_pdu[0] & 0x7f) == m->pending.fc;
        } else {
            pr = csmb_mbap_parser_feed(&m->conn->mbap, &in, &len);
            if (pr == CSMB_PR_NEED_MORE)
                break;
            if (pr == CSMB_PR_BAD)
                return -1;   /* desync: close and reconnect */
            r_pdu = m->conn->mbap.pdu;
            r_plen = m->conn->mbap.plen;
            r_unit = m->conn->mbap.unit;
            match = m->in_flight && m->conn->mbap.tid == m->cur_tid &&
                    r_unit == m->pending.unit;
        }

        m->timeout_streak = 0;   /* any complete frame proves the peer alive */

        if (match) {
            csmb_response r;

            if (csmb_decode_response(r_pdu, r_plen, m->pending.fc, &r) >= 0) {
                lws_sul_cancel(&m->resp_sul);
                m->in_flight = 0;
                reset_stale_timer(m, m->pending.unit);
                csmb_sched_on_response(&m->engine, &m->sched, &m->pending, &r,
                                       lws_now_usecs());
                processed = 1;
            } else if (!serial) {
                return -1;   /* TCP: a matched but malformed response is a desync */
            }
            /* serial: ignore the malformed frame, wait for the timeout */
        }

        if (serial)
            csmb_rtu_parser_reset(&m->conn->rtu);
        else
            csmb_mbap_parser_reset(&m->conn->mbap);
    }
    if (processed)
        master_pump(m);
    return 0;
}

/* ---- public API ---- */

csmb_master *csmb_master_create(struct lws_context *cx,
                                struct lws_vhost *client_vhost,
                                const csmb_transport *tr,
                                csmb_notify_cb notify, void *opaque)
{
    csmb_master *m;
    int i;

    if (tr->kind != CSMB_TR_TCP && tr->kind != CSMB_TR_SERIAL &&
        tr->kind != CSMB_TR_FD)
        return NULL;

    m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    csmb_engine_init(&m->engine, cx, notify, opaque);
    csmb_sched_init(&m->sched);
    m->client_vhost = client_vhost;
    m->tr = *tr;
    if (tr->host_or_device) {
        m->host = strdup(tr->host_or_device);
        if (!m->host) {
            csmb_sched_free(&m->sched);
            csmb_engine_free(&m->engine);
            free(m);
            return NULL;
        }
        m->tr.host_or_device = m->host;
    }
    m->heartbeat_ms = 1000;
    m->response_timeout_ms = 1000;
    m->conn_state = CSMB_CONN_OFFLINE;
    m->next_tid = 1;
    for (i = 0; i < 256; i++) {
        m->stale[i].m = m;
        m->stale[i].unit = (uint8_t)i;
    }
    /* Kick off the first connect on the next loop iteration, after the
     * Lisp side has stored the engine pointer (so the connecting event's
     * flush finds a fully-wired object). */
    lws_sul_schedule(cx, 0, &m->reconnect_sul, reconnect_cb, 0);
    return m;
}

void csmb_master_destroy(csmb_master *m)
{
    if (!m)
        return;
    m->engine.destroyed = 1;
    lws_sul_cancel(&m->reconnect_sul);
    lws_sul_cancel(&m->heartbeat_sul);
    lws_sul_cancel(&m->resp_sul);
    lws_sul_cancel(&m->pace_sul);
    lws_sul_cancel(&m->t35_sul);
    cancel_all_stale(m);
    if (m->conn) {
        m->conn->owner = NULL;
        if (m->conn->wsi)
            lws_set_timeout(m->conn->wsi, PENDING_TIMEOUT_HTTP_CONTENT,
                            LWS_TO_KILL_ASYNC);
        m->conn = NULL;
    }
    csmb_sched_free(&m->sched);
    csmb_engine_free(&m->engine);
    free(m->host);
    free(m);
}

int csmb_master_add_unit(csmb_master *m, uint8_t unit,
                         uint32_t request_delay_ms, uint32_t stale_timeout_ms,
                         uint32_t flags)
{
    return csmb_sched_add_unit(&m->sched, unit, request_delay_ms,
                               stale_timeout_ms, flags);
}

int32_t csmb_master_subscribe(csmb_master *m, uint8_t unit, int reg_type,
                              uint16_t start, uint16_t count, uint32_t flags)
{
    int32_t id = csmb_sched_subscribe(&m->sched, unit, reg_type, start, count,
                                      flags);
    if (id > 0) {
        master_kick(m);
        csmb_event_flush(&m->engine);
    }
    return id;
}

int csmb_master_unsubscribe(csmb_master *m, int32_t span_id)
{
    int rc = csmb_sched_unsubscribe(&m->sched, span_id);

    csmb_event_flush(&m->engine);
    return rc;
}

int csmb_master_refresh_span(csmb_master *m, int32_t span_id)
{
    int rc = csmb_sched_refresh_span(&m->sched, span_id);

    if (rc == CSMB_OK) {
        master_kick(m);
        csmb_event_flush(&m->engine);
    }
    return rc;
}

int csmb_master_set_unit_enabled(csmb_master *m, uint8_t unit, int enabled)
{
    int rc = csmb_sched_set_unit_enabled(&m->engine, &m->sched, unit, enabled);

    if (rc == CSMB_OK && enabled)
        master_kick(m);
    csmb_event_flush(&m->engine);
    return rc;
}

int csmb_master_set_poll_seq(csmb_master *m, uint8_t unit, int reg_type,
                             const csmb_range *ranges, size_t n)
{
    int rc = csmb_sched_set_poll_seq(&m->sched, unit, reg_type, ranges, n);

    if (rc == CSMB_OK) {
        master_kick(m);
        csmb_event_flush(&m->engine);
    }
    return rc;
}

int64_t csmb_master_write(csmb_master *m, uint8_t unit,
                          const csmb_write_spec *reqs, size_t n)
{
    int64_t op = csmb_sched_enqueue_write(&m->sched, unit, reqs, n);

    if (op > 0) {
        master_pump(m);
        csmb_event_flush(&m->engine);
    }
    return op;
}

void csmb_master_set_heartbeat(csmb_master *m, uint32_t ms)
{
    m->heartbeat_ms = ms ? ms : 1000;
    if (m->conn_state == CSMB_CONN_ONLINE)
        lws_sul_schedule(m->engine.cx, 0, &m->heartbeat_sul, heartbeat_cb,
                         (lws_usec_t)m->heartbeat_ms * 1000);
}

void csmb_master_set_response_timeout(csmb_master *m, uint32_t ms)
{
    m->response_timeout_ms = ms ? ms : 1000;
}
