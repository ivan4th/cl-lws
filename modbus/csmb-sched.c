/* Master scheduler: span store, poll-program bunching, change detection,
 * write FIFO and per-unit bookkeeping.
 *
 * Pure logic, no libwebsockets.  Events are emitted through a csmb_engine
 * (which works without lws); the master (csmb-master.c) owns the
 * transport, timers and request pump and drives everything here. */

#include "csmb-private.h"

/* ---- small helpers ---- */

static int reg_type_ok(int rt)
{
    return rt >= 0 && rt < CSMB_NUM_REG_TYPES;
}

static int reg_is_bits(int rt)
{
    return rt == CSMB_COIL || rt == CSMB_DISCRETE;
}

static uint16_t reg_max_read(int rt)
{
    return reg_is_bits(rt) ? CSMB_MAX_READ_FLAGS : CSMB_MAX_READ_WORDS;
}

static uint8_t reg_read_fc(int rt)
{
    switch (rt) {
    case CSMB_COIL:     return CSMB_FC_READ_COILS;
    case CSMB_DISCRETE: return CSMB_FC_READ_DISCRETE;
    case CSMB_HOLDING:  return CSMB_FC_READ_HOLDING;
    default:            return CSMB_FC_READ_INPUT;
    }
}

/* ---- event emitters ---- */

static void emit_span_update(csmb_engine *e, csmb_span *sp, const uint16_t *vals)
{
    CSMB_ZERO_EVENT(ev);
    ev.type = CSMB_EV_SPAN_UPDATE;
    ev.span_id = sp->id;
    ev.unit = sp->unit;
    ev.reg_type = sp->reg_type;
    ev.start = sp->start;
    csmb_event_add(e, &ev, vals, sp->count);   /* sets ev.count = sp->count */
}

static void emit_span_state(csmb_engine *e, csmb_span *sp, uint8_t state)
{
    CSMB_ZERO_EVENT(ev);
    ev.type = CSMB_EV_SPAN_STATE;
    ev.span_id = sp->id;
    ev.unit = sp->unit;
    ev.reg_type = sp->reg_type;
    ev.start = sp->start;
    ev.count = sp->count;
    ev.state = state;
    csmb_event_add(e, &ev, NULL, 0);
}

static void emit_unit_state(csmb_engine *e, uint8_t unit, uint8_t state)
{
    CSMB_ZERO_EVENT(ev);
    ev.type = CSMB_EV_UNIT_STATE;
    ev.unit = unit;
    ev.state = state;
    csmb_event_add(e, &ev, NULL, 0);
}

static void emit_write_done(csmb_engine *e, int64_t op_id, uint8_t unit,
                            uint8_t status, uint8_t exc, uint8_t aux)
{
    CSMB_ZERO_EVENT(ev);
    ev.type = CSMB_EV_WRITE_DONE;
    ev.op_id = op_id;
    ev.unit = unit;
    ev.state = status;
    ev.exception = exc;
    ev.aux = aux;
    csmb_event_add(e, &ev, NULL, 0);
}

static void emit_log(csmb_engine *e, uint8_t unit, uint8_t kind, uint8_t fc,
                     uint8_t exc)
{
    CSMB_ZERO_EVENT(ev);
    ev.type = CSMB_EV_LOG;
    ev.unit = unit;
    ev.state = kind;
    ev.aux = fc;
    ev.exception = exc;
    csmb_event_add(e, &ev, NULL, 0);
}

#define CSMB_LOG_RATE_US ((csmb_usec_t)5000000)   /* one LOG per (unit,kind)/5s */

/* Rate-limited LOG: at most one per (unit, kind) per CSMB_LOG_RATE_US. */
static void emit_log_rl(csmb_engine *e, csmb_sched *sc, uint8_t unit,
                        uint8_t kind, uint8_t fc, uint8_t exc, csmb_usec_t now)
{
    csmb_sunit *u = csmb_sched_find_unit(sc, unit);

    if (u && kind < 3) {
        if (u->last_log_us[kind] != 0 && now - u->last_log_us[kind] < CSMB_LOG_RATE_US)
            return;
        u->last_log_us[kind] = now ? now : 1;   /* 0 means "never logged" */
    }
    emit_log(e, unit, kind, fc, exc);
}

/* ---- units ---- */

csmb_sunit *csmb_sched_find_unit(csmb_sched *sc, uint8_t unit)
{
    csmb_sunit *u;

    for (u = sc->units; u; u = u->next)
        if (u->unit == unit)
            return u;
    return NULL;
}

static csmb_sunit *ensure_unit(csmb_sched *sc, uint8_t unit)
{
    csmb_sunit *u = csmb_sched_find_unit(sc, unit);
    csmb_sunit **pp;

    if (u)
        return u;
    u = calloc(1, sizeof(*u));
    if (!u)
        return NULL;
    u->unit = unit;
    u->enabled = 1;
    u->stale_timeout_ms = 20000;
    /* append to keep a stable, add-order round-robin */
    for (pp = &sc->units; *pp; pp = &(*pp)->next)
        ;
    *pp = u;
    return u;
}

int csmb_sched_add_unit(csmb_sched *sc, uint8_t unit, uint32_t request_delay_ms,
                        uint32_t stale_timeout_ms, uint32_t flags)
{
    csmb_sunit *u = ensure_unit(sc, unit);

    if (!u)
        return CSMB_ENOMEM;
    u->request_delay_ms = request_delay_ms;
    u->stale_timeout_ms = stale_timeout_ms ? stale_timeout_ms : 20000;
    u->flags = flags;
    return CSMB_OK;
}

/* ---- lifecycle ---- */

void csmb_sched_init(csmb_sched *sc)
{
    memset(sc, 0, sizeof(*sc));
    sc->next_span_id = 1;
    sc->next_op_id = 1;
}

static void free_write_op(csmb_write_op *op)
{
    free(op->reqs);
    free(op);
}

void csmb_sched_free(csmb_sched *sc)
{
    csmb_sunit *u = sc->units;
    csmb_span *sp = sc->spans;
    csmb_write_op *op = sc->wq_head;

    while (u) {
        csmb_sunit *un = u->next;
        int rt;

        for (rt = 0; rt < CSMB_NUM_REG_TYPES; rt++)
            free(u->poll_seq[rt]);
        free(u);
        u = un;
    }
    while (sp) {
        csmb_span *spn = sp->next;

        free(sp->values);
        free(sp);
        sp = spn;
    }
    while (op) {
        csmb_write_op *opn = op->next;

        free_write_op(op);
        op = opn;
    }
    free(sc->program);
    memset(sc, 0, sizeof(*sc));
}

/* ---- subscriptions ---- */

static int ranges_intersect(uint16_t a0, uint16_t ac, uint16_t b0, uint16_t bc)
{
    uint32_t a1 = (uint32_t)a0 + ac, b1 = (uint32_t)b0 + bc;

    return a0 < b1 && b0 < a1;
}

int32_t csmb_sched_subscribe(csmb_sched *sc, uint8_t unit, int reg_type,
                             uint16_t start, uint16_t count, uint32_t flags)
{
    csmb_span *ns;

    if (!reg_type_ok(reg_type))
        return CSMB_EBADTYPE;
    if (count == 0 || (uint32_t)start + count > 0x10000u)
        return CSMB_ERANGE;
    if (count > reg_max_read(reg_type))
        return CSMB_ETOOBIG;

    /* Overlapping spans are allowed: the poll program merges
     * intersecting ranges into covering reads (csmb_bunch_ranges) and
     * every span covered by a completed read step extracts its own
     * window with per-span change detection.  Real param maps do
     * contain overlapping register windows (e.g. two 32-bit values
     * sharing a register). */
    if (!ensure_unit(sc, unit))
        return CSMB_ENOMEM;

    ns = calloc(1, sizeof(*ns));
    if (!ns)
        return CSMB_ENOMEM;
    ns->values = calloc(count, sizeof(uint16_t));
    if (!ns->values) {
        free(ns);
        return CSMB_ENOMEM;
    }
    ns->id = sc->next_span_id++;
    ns->unit = unit;
    ns->reg_type = (uint8_t)reg_type;
    ns->start = start;
    ns->count = count;
    ns->flags = flags;
    ns->next = sc->spans;
    sc->spans = ns;
    return ns->id;
}

int csmb_sched_unsubscribe(csmb_sched *sc, int32_t span_id)
{
    csmb_span **pp = &sc->spans;

    while (*pp) {
        if ((*pp)->id == span_id) {
            csmb_span *dead = *pp;

            *pp = dead->next;
            free(dead->values);
            free(dead);
            return CSMB_OK;
        }
        pp = &(*pp)->next;
    }
    return CSMB_ENOSPAN;
}

int csmb_sched_refresh_span(csmb_sched *sc, int32_t span_id)
{
    csmb_span *sp;

    for (sp = sc->spans; sp; sp = sp->next)
        if (sp->id == span_id) {
            sp->force = 1;
            return CSMB_OK;
        }
    return CSMB_ENOSPAN;
}

int csmb_sched_set_poll_seq(csmb_sched *sc, uint8_t unit, int reg_type,
                            const csmb_range *ranges, size_t n)
{
    csmb_sunit *u;
    csmb_range *copy = NULL;

    if (!reg_type_ok(reg_type))
        return CSMB_EBADTYPE;
    u = ensure_unit(sc, unit);
    if (!u)
        return CSMB_ENOMEM;
    if (n) {
        copy = malloc(n * sizeof(*copy));
        if (!copy)
            return CSMB_ENOMEM;
        memcpy(copy, ranges, n * sizeof(*copy));
    }
    free(u->poll_seq[reg_type]);
    u->poll_seq[reg_type] = copy;
    u->poll_seq_n[reg_type] = n;
    return CSMB_OK;
}

/* ---- write queue ---- */

/* Build one write PDU from a write_spec into WR.  Returns 0 or a negative
 * CSMB_E* error. */
static int build_wreq(csmb_wreq *wr, uint32_t unit_flags,
                      const csmb_write_spec *spec)
{
    int rt = spec->reg_type;
    int plen;

    if (rt != CSMB_COIL && rt != CSMB_HOLDING)
        return CSMB_EBADTYPE;

    wr->reg_type = (uint8_t)rt;
    wr->start = spec->start;
    wr->count = spec->count;

    if (spec->count == 1 && !(unit_flags & CSMB_UNIT_FC1516_ONLY)) {
        uint16_t v = spec->values[0];

        plen = csmb_build_write_single(wr->pdu, rt, spec->start, v);
        if (plen < 0)
            return plen;
        wr->fc = (rt == CSMB_COIL) ? CSMB_FC_WRITE_SINGLE_COIL
                                   : CSMB_FC_WRITE_SINGLE_REG;
        wr->verify = (rt == CSMB_COIL) ? (uint16_t)(v ? 0xFF00 : 0x0000) : v;
    } else {
        plen = csmb_build_write_multi(wr->pdu, rt, spec->start, spec->count,
                                      spec->values);
        if (plen < 0)
            return plen;
        wr->fc = (rt == CSMB_COIL) ? CSMB_FC_WRITE_MULTI_COILS
                                   : CSMB_FC_WRITE_MULTI_REGS;
        wr->verify = spec->count;
    }
    wr->plen = (uint16_t)plen;
    return 0;
}

int64_t csmb_sched_enqueue_write(csmb_sched *sc, uint8_t unit,
                                 const csmb_write_spec *reqs, size_t n)
{
    csmb_sunit *u;
    csmb_write_op *op;
    size_t i;
    int rc;

    if (n == 0)
        return CSMB_EINVAL;
    u = ensure_unit(sc, unit);
    if (!u)
        return CSMB_ENOMEM;

    op = calloc(1, sizeof(*op));
    if (!op)
        return CSMB_ENOMEM;
    op->reqs = calloc(n, sizeof(*op->reqs));
    if (!op->reqs) {
        free(op);
        return CSMB_ENOMEM;
    }
    for (i = 0; i < n; i++) {
        rc = build_wreq(&op->reqs[i], u->flags, &reqs[i]);
        if (rc < 0) {
            free_write_op(op);
            return rc;
        }
    }
    op->op_id = sc->next_op_id++;
    op->unit = unit;
    op->nreqs = (int)n;
    op->cur = 0;
    if (sc->wq_tail)
        sc->wq_tail->next = op;
    else
        sc->wq_head = op;
    sc->wq_tail = op;
    return op->op_id;
}

static void pop_write_op(csmb_sched *sc)
{
    csmb_write_op *op = sc->wq_head;

    if (!op)
        return;
    sc->wq_head = op->next;
    if (!sc->wq_head)
        sc->wq_tail = NULL;
    free_write_op(op);
}

/* Fail and drop every queued write op for UNIT (or all units if
 * unit_only is 0), emitting WRITE_DONE with STATUS. */
static void fail_writes(csmb_engine *e, csmb_sched *sc, int unit_only,
                        uint8_t unit, uint8_t status)
{
    csmb_write_op **pp = &sc->wq_head;

    sc->wq_tail = NULL;
    while (*pp) {
        csmb_write_op *op = *pp;

        if (!unit_only || op->unit == unit) {
            *pp = op->next;
            emit_write_done(e, op->op_id, op->unit, status, 0, (uint8_t)op->cur);
            free_write_op(op);
        } else {
            sc->wq_tail = op;
            pp = &op->next;
        }
    }
}

/* ---- bunching ---- */

typedef struct { uint32_t s, e; } iv_t;

static int iv_cmp(const void *a, const void *b)
{
    uint32_t sa = ((const iv_t *)a)->s, sb = ((const iv_t *)b)->s;

    return (sa > sb) - (sa < sb);
}

size_t csmb_bunch_ranges(int reg_type, const csmb_range *in, size_t n,
                         csmb_range *out, size_t out_cap)
{
    iv_t *iv;
    size_t i, m, count = 0;
    uint32_t max_size = reg_max_read(reg_type);
    uint32_t max_dummy = reg_is_bits(reg_type)
                         ? (uint32_t)CSMB_MAX_DUMMY_BYTES * 8
                         : (uint32_t)CSMB_MAX_DUMMY_BYTES / 2;
    uint32_t bs = 0, be = 0;
    int have = 0;

    if (n == 0)
        return 0;
    iv = malloc(n * sizeof(*iv));
    if (!iv)
        return 0;
    for (i = 0; i < n; i++) {
        iv[i].s = in[i].start;
        iv[i].e = (uint32_t)in[i].start + in[i].count;
    }
    qsort(iv, n, sizeof(*iv), iv_cmp);

    /* merge overlapping / touching intervals */
    m = 0;
    for (i = 1; i < n; i++) {
        if (iv[i].s <= iv[m].e) {
            if (iv[i].e > iv[m].e)
                iv[m].e = iv[i].e;
        } else {
            iv[++m] = iv[i];
        }
    }
    m++;   /* number of merged intervals */

    /* greedily form bunches: merge across small gaps, split oversized */
    for (i = 0; i < m; i++) {
        if (!have) {
            bs = iv[i].s;
            be = iv[i].e;
            have = 1;
        } else if (iv[i].s - be <= max_dummy && iv[i].e - bs <= max_size) {
            be = iv[i].e;
        } else {
            if (out && count < out_cap) {
                out[count].start = (uint16_t)bs;
                out[count].count = (uint16_t)(be - bs);
            }
            count++;
            bs = iv[i].s;
            be = iv[i].e;
        }
        while (be - bs > max_size) {
            if (out && count < out_cap) {
                out[count].start = (uint16_t)bs;
                out[count].count = (uint16_t)max_size;
            }
            count++;
            bs += max_size;
        }
    }
    if (have) {
        if (out && count < out_cap) {
            out[count].start = (uint16_t)bs;
            out[count].count = (uint16_t)(be - bs);
        }
        count++;
    }
    free(iv);
    return count;
}

/* ---- poll program ---- */

static int prog_push(csmb_sched *sc, uint8_t unit, uint8_t rt,
                     uint16_t start, uint16_t count)
{
    if (sc->prog_len == sc->prog_cap) {
        size_t ncap = sc->prog_cap ? sc->prog_cap * 2 : 16;
        csmb_readstep *np = realloc(sc->program, ncap * sizeof(*np));

        if (!np)
            return CSMB_ENOMEM;
        sc->program = np;
        sc->prog_cap = ncap;
    }
    sc->program[sc->prog_len].unit = unit;
    sc->program[sc->prog_len].reg_type = rt;
    sc->program[sc->prog_len].start = start;
    sc->program[sc->prog_len].count = count;
    sc->prog_len++;
    return CSMB_OK;
}

static int span_covered_by_seq(const csmb_range *seq, size_t n,
                               uint16_t start, uint16_t count)
{
    size_t i;

    for (i = 0; i < n; i++) {
        uint32_t rs = seq[i].start, re = (uint32_t)seq[i].start + seq[i].count;

        if (start >= rs && (uint32_t)start + count <= re)
            return 1;
    }
    return 0;
}

/* Build the read steps for one (unit, reg_type). */
static void build_unit_regtype(csmb_engine *e, csmb_sched *sc, csmb_sunit *u,
                               int rt)
{
    csmb_span *sp;

    if (u->poll_seq[rt]) {
        size_t i;

        for (i = 0; i < u->poll_seq_n[rt]; i++)
            prog_push(sc, u->unit, (uint8_t)rt,
                      u->poll_seq[rt][i].start, u->poll_seq[rt][i].count);
        /* one-shot UNCOVERED for spans the explicit program misses */
        for (sp = sc->spans; sp; sp = sp->next) {
            if (sp->unit != u->unit || sp->reg_type != rt)
                continue;
            if (span_covered_by_seq(u->poll_seq[rt], u->poll_seq_n[rt],
                                    sp->start, sp->count)) {
                if (sp->state == CSMB_ST_UNCOVERED)
                    sp->state = 0;
            } else if (sp->state != CSMB_ST_UNCOVERED) {
                sp->state = CSMB_ST_UNCOVERED;
                emit_span_state(e, sp, CSMB_ST_UNCOVERED);
            }
        }
    } else {
        csmb_range tmp[64];
        csmb_range *ranges = tmp;
        size_t nspan = 0, cap = sizeof(tmp) / sizeof(tmp[0]), nb, i;

        for (sp = sc->spans; sp; sp = sp->next)
            if (sp->unit == u->unit && sp->reg_type == rt)
                nspan++;
        if (nspan == 0)
            return;
        if (nspan > cap) {
            ranges = malloc(nspan * sizeof(*ranges));
            if (!ranges)
                return;
        }
        i = 0;
        for (sp = sc->spans; sp; sp = sp->next)
            if (sp->unit == u->unit && sp->reg_type == rt) {
                ranges[i].start = sp->start;
                ranges[i].count = sp->count;
                i++;
                if (sp->state == CSMB_ST_UNCOVERED)
                    sp->state = 0;   /* covered again after clearing a poll-seq */
            }
        nb = csmb_bunch_ranges(rt, ranges, nspan, ranges, nspan);
        for (i = 0; i < nb; i++)
            prog_push(sc, u->unit, (uint8_t)rt, ranges[i].start, ranges[i].count);
        if (ranges != tmp)
            free(ranges);
    }
}

void csmb_sched_start_round(csmb_engine *e, csmb_sched *sc)
{
    int pass, rt;

    sc->prog_len = 0;
    sc->prog_idx = 0;

    /* healthy units first, failing units last */
    for (pass = 0; pass < 2; pass++) {
        csmb_sunit *u;

        for (u = sc->units; u; u = u->next) {
            if (!u->enabled)
                continue;
            if ((pass == 0) == (u->failing != 0))
                continue;
            for (rt = 0; rt < CSMB_NUM_REG_TYPES; rt++)
                build_unit_regtype(e, sc, u, rt);
        }
    }
}

/* ---- request pump ---- */

int csmb_sched_pick(csmb_sched *sc, csmb_pending *p)
{
    csmb_write_op *op = sc->wq_head;

    /* writes preempt reads and run to completion */
    if (op && op->cur < op->nreqs) {
        csmb_wreq *wr = &op->reqs[op->cur];

        memset(p, 0, sizeof(*p));
        p->kind = CSMB_PICK_WRITE;
        p->unit = op->unit;
        p->fc = wr->fc;
        p->reg_type = wr->reg_type;
        p->start = wr->start;
        p->count = wr->count;
        memcpy(p->pdu, wr->pdu, wr->plen);
        p->plen = wr->plen;
        p->op_id = op->op_id;
        p->req_index = op->cur;
        return CSMB_PICK_WRITE;
    }

    while (sc->prog_idx < sc->prog_len) {
        csmb_readstep *st = &sc->program[sc->prog_idx++];
        csmb_sunit *u = csmb_sched_find_unit(sc, st->unit);
        int plen;

        if (!u || !u->enabled)
            continue;   /* disabled mid-round */
        memset(p, 0, sizeof(*p));
        plen = csmb_build_read(p->pdu, st->reg_type, st->start, st->count);
        if (plen < 0)
            continue;
        p->kind = CSMB_PICK_READ;
        p->unit = st->unit;
        p->fc = reg_read_fc(st->reg_type);
        p->reg_type = st->reg_type;
        p->start = st->start;
        p->count = st->count;
        p->plen = (uint16_t)plen;
        return CSMB_PICK_READ;
    }
    return CSMB_PICK_NONE;
}

/* ---- response handling ---- */

static int span_has_pending_write(csmb_sched *sc, const csmb_span *sp)
{
    csmb_write_op *op;
    int i;

    for (op = sc->wq_head; op; op = op->next) {
        if (op->unit != sp->unit)
            continue;
        for (i = 0; i < op->nreqs; i++)
            if (op->reqs[i].reg_type == sp->reg_type &&
                ranges_intersect(op->reqs[i].start, op->reqs[i].count,
                                 sp->start, sp->count))
                return 1;
    }
    return 0;
}

/* Does response R (a read covering [rd_start, rd_start+rd_count)) hold
 * enough bytes to extract SP? */
static int response_covers(const csmb_response *r, int rt, uint16_t rd_start,
                           const csmb_span *sp)
{
    uint32_t off = (uint32_t)sp->start - rd_start;
    uint32_t end = off + sp->count;

    if (reg_is_bits(rt))
        return (end + 7) / 8 <= r->nbytes;
    return end * 2 <= r->nbytes;
}

static void extract_span(int rt, const csmb_response *r, uint16_t rd_start,
                         const csmb_span *sp, uint16_t *out)
{
    uint16_t off = (uint16_t)(sp->start - rd_start);
    uint16_t i;

    for (i = 0; i < sp->count; i++) {
        uint16_t idx = (uint16_t)(off + i);

        if (reg_is_bits(rt))
            out[i] = (uint16_t)((r->data[idx / 8] >> (idx % 8)) & 1);
        else
            out[i] = (uint16_t)((r->data[2 * idx] << 8) | r->data[2 * idx + 1]);
    }
}

/* On write-op completion (success OR failure) force the next successful
 * read of every overlapping span to publish, so the model reconverges to
 * the device-actual value even if it equals the pre-write image (the
 * device may clamp/ignore the write, or the op may have timed out). */
static void force_overlapping_spans(csmb_sched *sc, const csmb_write_op *op)
{
    csmb_span *sp;
    int i;

    for (sp = sc->spans; sp; sp = sp->next) {
        if (sp->unit != op->unit)
            continue;
        for (i = 0; i < op->nreqs; i++)
            if (op->reqs[i].reg_type == sp->reg_type &&
                ranges_intersect(op->reqs[i].start, op->reqs[i].count,
                                 sp->start, sp->count)) {
                sp->force = 1;
                break;
            }
    }
}

static void handle_write_response(csmb_engine *e, csmb_sched *sc,
                                  const csmb_pending *p, const csmb_response *r)
{
    csmb_write_op *op = sc->wq_head;
    csmb_sunit *u = csmb_sched_find_unit(sc, p->unit);
    csmb_wreq *wr;

    if (!op || op->op_id != p->op_id || op->cur != p->req_index)
        return;   /* stale (op was dropped) */
    wr = &op->reqs[op->cur];

    if (r->exception) {
        emit_write_done(e, op->op_id, op->unit, CSMB_WR_EXCEPTION,
                        r->exception, (uint8_t)op->cur);
        force_overlapping_spans(sc, op);
        pop_write_op(sc);
        return;
    }
    if (!(u && (u->flags & CSMB_UNIT_NO_VERIFY_WRITE))) {
        if (r->start != wr->start || r->count_or_value != wr->verify) {
            emit_write_done(e, op->op_id, op->unit, CSMB_WR_VERIFY_FAILED,
                            0, (uint8_t)op->cur);
            force_overlapping_spans(sc, op);
            pop_write_op(sc);
            return;
        }
    }
    op->cur++;
    if (op->cur >= op->nreqs) {
        emit_write_done(e, op->op_id, op->unit, CSMB_WR_OK, 0, 0);
        force_overlapping_spans(sc, op);
        pop_write_op(sc);
    }
}

void csmb_sched_on_response(csmb_engine *e, csmb_sched *sc,
                            const csmb_pending *p, const csmb_response *r,
                            csmb_usec_t now)
{
    csmb_sunit *u = csmb_sched_find_unit(sc, p->unit);
    csmb_span *sp;

    /* the unit answered (data OR exception): mark it online and record
     * the response time (for the unit-offline sweep) */
    if (u) {
        u->last_response = now;
        if (!u->online) {
            u->online = 1;
            emit_unit_state(e, u->unit, CSMB_ST_ONLINE);
            for (sp = sc->spans; sp; sp = sp->next)
                if (sp->unit == u->unit) {
                    sp->have_values = 0;   /* republish on recovery */
                    if (sp->state == CSMB_ST_STALE || sp->state == CSMB_ST_OFFLINE)
                        sp->state = 0;
                }
        }
        u->failing = 0;
    }

    if (p->kind == CSMB_PICK_WRITE) {
        handle_write_response(e, sc, p, r);
        return;
    }

    /* read exception: the unit is alive but this read yielded no data, so
     * the covered spans' last_ok simply does not advance — they go stale
     * only if the exceptions persist past the stale-timeout (the sweep) */
    if (r->exception) {
        emit_log_rl(e, sc, p->unit, CSMB_LOG_EXCEPTION, p->fc, r->exception, now);
        return;
    }

    for (sp = sc->spans; sp; sp = sp->next) {
        uint16_t vals[CSMB_MAX_READ_FLAGS];

        if (sp->unit != p->unit || sp->reg_type != p->reg_type)
            continue;
        if (sp->start < p->start ||
            (uint32_t)sp->start + sp->count > (uint32_t)p->start + p->count)
            continue;   /* not covered by this read step */
        if (span_has_pending_write(sc, sp))
            continue;   /* held until the write completes */
        if (!response_covers(r, sp->reg_type, p->start, sp))
            continue;   /* short response */

        sp->last_ok = now;   /* a successful read of THIS span */
        extract_span(sp->reg_type, r, p->start, sp, vals);
        if (sp->state != CSMB_ST_ONLINE) {
            sp->state = CSMB_ST_ONLINE;
            emit_span_state(e, sp, CSMB_ST_ONLINE);
        }
        if (!sp->have_values || sp->force || (sp->flags & CSMB_SPAN_ALWAYS) ||
            memcmp(sp->values, vals, sp->count * sizeof(uint16_t)) != 0) {
            memcpy(sp->values, vals, sp->count * sizeof(uint16_t));
            sp->have_values = 1;
            sp->force = 0;
            emit_span_update(e, sp, vals);
        }
    }
}

void csmb_sched_on_request_failed(csmb_engine *e, csmb_sched *sc,
                                  const csmb_pending *p, int wr_status,
                                  csmb_usec_t now)
{
    if (p->kind == CSMB_PICK_WRITE) {
        csmb_write_op *op = sc->wq_head;

        if (op && op->op_id == p->op_id) {
            emit_write_done(e, op->op_id, op->unit, (uint8_t)wr_status, 0,
                            (uint8_t)op->cur);
            force_overlapping_spans(sc, op);
            pop_write_op(sc);
        }
    } else {
        csmb_sunit *u = csmb_sched_find_unit(sc, p->unit);

        if (u)
            u->failing = 1;
        emit_log_rl(e, sc, p->unit, CSMB_LOG_UNIT_TIMEOUT, p->fc, 0, now);
    }
}

uint32_t csmb_sched_min_stale_ms(csmb_sched *sc)
{
    csmb_sunit *u;
    uint32_t min = 0;

    for (u = sc->units; u; u = u->next)
        if (min == 0 || u->stale_timeout_ms < min)
            min = u->stale_timeout_ms;
    return min ? min : 20000;
}

void csmb_sched_staleness_sweep(csmb_engine *e, csmb_sched *sc, csmb_usec_t now)
{
    csmb_sunit *u;
    csmb_span *sp;

    /* a unit silent past its stale-timeout goes offline */
    for (u = sc->units; u; u = u->next) {
        csmb_usec_t to = (csmb_usec_t)u->stale_timeout_ms * 1000;

        if (u->online && u->last_response != 0 && now - u->last_response > to) {
            emit_unit_state(e, u->unit, CSMB_ST_OFFLINE);
            u->online = 0;
        }
    }

    /* a span not read successfully within its unit's stale-timeout goes
     * stale; never-read spans are baselined to the first sweep (~ subscribe
     * / connect time) so they, too, stale after the timeout */
    for (sp = sc->spans; sp; sp = sp->next) {
        csmb_sunit *su = csmb_sched_find_unit(sc, sp->unit);
        csmb_usec_t to = (csmb_usec_t)(su ? su->stale_timeout_ms : 20000) * 1000;

        if (sp->last_ok == 0) {
            sp->last_ok = now;
            continue;
        }
        if (now - sp->last_ok > to &&
            sp->state != CSMB_ST_STALE && sp->state != CSMB_ST_OFFLINE &&
            sp->state != CSMB_ST_UNCOVERED) {
            sp->state = CSMB_ST_STALE;
            sp->have_values = 0;
            emit_span_state(e, sp, CSMB_ST_STALE);
        }
    }
}

int csmb_sched_set_unit_enabled(csmb_engine *e, csmb_sched *sc, uint8_t unit,
                                int enabled)
{
    csmb_sunit *u = csmb_sched_find_unit(sc, unit);
    csmb_span *sp;

    if (!u)
        return CSMB_ENOUNIT;
    enabled = enabled ? 1 : 0;
    if (u->enabled == enabled)
        return CSMB_OK;
    u->enabled = enabled;

    if (!enabled) {
        fail_writes(e, sc, 1, unit, CSMB_WR_UNIT_DISABLED);
        if (u->online) {
            emit_unit_state(e, unit, CSMB_ST_OFFLINE);
            u->online = 0;
        }
        for (sp = sc->spans; sp; sp = sp->next)
            if (sp->unit == unit && sp->state != CSMB_ST_STALE &&
                sp->state != CSMB_ST_OFFLINE) {
                sp->state = CSMB_ST_STALE;
                emit_span_state(e, sp, CSMB_ST_STALE);
            }
    } else {
        for (sp = sc->spans; sp; sp = sp->next)
            if (sp->unit == unit)
                sp->have_values = 0;   /* republish once re-enabled */
    }
    return CSMB_OK;
}

void csmb_sched_connection_down(csmb_engine *e, csmb_sched *sc)
{
    csmb_sunit *u;
    csmb_span *sp;

    fail_writes(e, sc, 0, 0, CSMB_WR_CONN_FAILED);
    for (u = sc->units; u; u = u->next) {
        if (u->online) {
            emit_unit_state(e, u->unit, CSMB_ST_OFFLINE);
            u->online = 0;
        }
        u->failing = 0;
    }
    for (sp = sc->spans; sp; sp = sp->next) {
        if (sp->state != CSMB_ST_OFFLINE) {
            sp->state = CSMB_ST_OFFLINE;
            sp->have_values = 0;
            emit_span_state(e, sp, CSMB_ST_OFFLINE);
        }
        sp->last_ok = 0;
    }
    for (u = sc->units; u; u = u->next)
        u->last_response = 0;
    sc->prog_len = sc->prog_idx = 0;
}

void csmb_sched_connection_up(csmb_sched *sc)
{
    csmb_sunit *u;
    csmb_span *sp;

    for (sp = sc->spans; sp; sp = sp->next) {
        sp->have_values = 0;
        sp->state = 0;
        sp->last_ok = 0;   /* re-baseline on (re)connect */
    }
    for (u = sc->units; u; u = u->next) {
        u->online = 0;
        u->failing = 0;
        u->last_response = 0;
    }
    sc->prog_len = sc->prog_idx = 0;
}
