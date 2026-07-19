/* Master scheduler tests: range bunching, subscribe/overlap, poll-program
 * reads with change detection (first/unchanged/changed/refresh/always),
 * coil reads, write FIFO (ok/exception/verify/no-verify/fc1516/multi-
 * request), read hold during a pending write, poll-seq coverage +
 * :uncovered, enable/disable, staleness and connection down/up.  Pure C. */

#include "../csmb-private.h"
#include "test-util.h"

/* ---- captured events ---- */

struct evrec {
    uint8_t  type, unit, reg_type, state, exception, aux;
    uint16_t start, count;
    int32_t  span_id;
    int64_t  op_id;
    uint16_t vals[64];
    int      has_vals;
};

static struct evrec cap[64];
static int ncap;

static void capture(csmb_engine *e)
{
    const csmb_event *evs;
    size_t n, i, j;

    ncap = 0;
    n = csmb_events_get(e, &evs);
    for (i = 0; i < n && ncap < 64; i++) {
        struct evrec *c = &cap[ncap++];

        c->type = evs[i].type;
        c->unit = evs[i].unit;
        c->reg_type = evs[i].reg_type;
        c->state = evs[i].state;
        c->exception = evs[i].exception;
        c->aux = evs[i].aux;
        c->start = evs[i].start;
        c->count = evs[i].count;
        c->span_id = evs[i].span_id;
        c->op_id = evs[i].op_id;
        c->has_vals = evs[i].values != NULL;
        if (evs[i].values)
            for (j = 0; j < evs[i].count && j < 64; j++)
                c->vals[j] = evs[i].values[j];
    }
    csmb_events_done(e);
}

static int count_type(uint8_t type)
{
    int i, k = 0;

    for (i = 0; i < ncap; i++)
        if (cap[i].type == type)
            k++;
    return k;
}

static struct evrec *nth_type(uint8_t type, int nth)
{
    int i, k = 0;

    for (i = 0; i < ncap; i++)
        if (cap[i].type == type && k++ == nth)
            return &cap[i];
    return NULL;
}

/* ---- response builders (through the real codec) ---- */

static uint8_t codbuf[300];

static uint8_t rd_fc(int rt)
{
    return rt == CSMB_COIL ? 1 : rt == CSMB_DISCRETE ? 2 :
           rt == CSMB_HOLDING ? 3 : 4;
}

static csmb_response resp_read(int rt, const uint16_t *vals, uint16_t count)
{
    csmb_response r;
    int coils = (rt == CSMB_COIL || rt == CSMB_DISCRETE);
    uint8_t fc = rd_fc(rt);
    int plen = csmb_build_read_response(codbuf, fc, vals, count, coils);

    TCHECK(plen > 0);
    TCHECK_EQ(csmb_decode_response(codbuf, (size_t)plen, fc, &r), 0);
    return r;
}

static csmb_response resp_echo(uint8_t fc, uint16_t start, uint16_t cv)
{
    csmb_response r;
    int plen = csmb_build_write_response(codbuf, fc, start, cv);

    TCHECK_EQ(csmb_decode_response(codbuf, (size_t)plen, fc, &r), 0);
    return r;
}

static csmb_response resp_exc(uint8_t fc, uint8_t exc)
{
    csmb_response r;
    int plen = csmb_build_exception(codbuf, fc, exc);

    TCHECK_EQ(csmb_decode_response(codbuf, (size_t)plen, fc, &r), 0);
    return r;
}

/* Run one full read round of a single-step program and deliver VALS. */
static void one_read(csmb_engine *e, csmb_sched *sc, int rt,
                     const uint16_t *vals, uint16_t count)
{
    csmb_pending p;
    csmb_response r;

    csmb_sched_start_round(e, sc);
    TCHECK_EQ(csmb_sched_pick(sc, &p), CSMB_PICK_READ);
    TCHECK_EQ(p.reg_type, rt);
    TCHECK_EQ(p.count, count);
    r = resp_read(rt, vals, count);
    csmb_sched_on_response(e, sc, &p, &r, 0);
}

/* ---- bunching ---- */

static void test_bunch(void)
{
    csmb_range in[4], out[8];
    size_t n;

    in[0].start = 0;  in[0].count = 10;
    in[1].start = 15; in[1].count = 5;    /* gap 5 <= 8 -> merge */
    n = csmb_bunch_ranges(CSMB_HOLDING, in, 2, out, 8);
    TCHECK_EQ(n, 1);
    TCHECK_EQ(out[0].start, 0);
    TCHECK_EQ(out[0].count, 20);

    in[1].start = 20; in[1].count = 5;    /* gap 10 > 8 -> split */
    n = csmb_bunch_ranges(CSMB_HOLDING, in, 2, out, 8);
    TCHECK_EQ(n, 2);
    TCHECK_EQ(out[0].count, 10);
    TCHECK_EQ(out[1].start, 20);
    TCHECK_EQ(out[1].count, 5);

    in[0].start = 0;   in[0].count = 125;
    in[1].start = 125; in[1].count = 125; /* touching -> [0,250), split at 125 */
    n = csmb_bunch_ranges(CSMB_HOLDING, in, 2, out, 8);
    TCHECK_EQ(n, 2);
    TCHECK_EQ(out[0].count, 125);
    TCHECK_EQ(out[1].start, 125);
    TCHECK_EQ(out[1].count, 125);

    in[0].start = 0;   in[0].count = 10;
    in[1].start = 130; in[1].count = 10;  /* coils: gap 120 <= 128 -> merge */
    n = csmb_bunch_ranges(CSMB_COIL, in, 2, out, 8);
    TCHECK_EQ(n, 1);
    TCHECK_EQ(out[0].count, 140);
    in[1].start = 140; in[1].count = 10;  /* gap 130 > 128 -> split */
    n = csmb_bunch_ranges(CSMB_COIL, in, 2, out, 8);
    TCHECK_EQ(n, 2);

    in[0].start = 100; in[0].count = 5;   /* unsorted input */
    in[1].start = 0;   in[1].count = 5;
    in[2].start = 50;  in[2].count = 5;
    n = csmb_bunch_ranges(CSMB_HOLDING, in, 3, out, 8);
    TCHECK_EQ(n, 3);
    TCHECK_EQ(out[0].start, 0);
    TCHECK_EQ(out[1].start, 50);
    TCHECK_EQ(out[2].start, 100);
}

/* ---- subscribe ---- */

static void test_subscribe(void)
{
    csmb_sched sc;
    int32_t a, b;

    csmb_sched_init(&sc);
    a = csmb_sched_subscribe(&sc, 1, CSMB_HOLDING, 10, 5, 0);
    TCHECK(a > 0);
    /* overlapping is allowed (real param maps contain overlapping
     * register windows; the poll program merges them) */
    TCHECK(csmb_sched_subscribe(&sc, 1, CSMB_HOLDING, 12, 5, 0) > 0);
    b = csmb_sched_subscribe(&sc, 1, CSMB_HOLDING, 10, 5, 0);  /* identical ok */
    TCHECK(b > 0 && b != a);
    TCHECK(csmb_sched_subscribe(&sc, 1, CSMB_COIL, 10, 5, 0) > 0);
    TCHECK(csmb_sched_subscribe(&sc, 2, CSMB_HOLDING, 12, 5, 0) > 0);
    TCHECK_EQ(csmb_sched_subscribe(&sc, 1, CSMB_HOLDING, 0, 0, 0), CSMB_ERANGE);
    TCHECK_EQ(csmb_sched_subscribe(&sc, 1, CSMB_HOLDING, 0,
                                   CSMB_MAX_READ_WORDS + 1, 0), CSMB_ETOOBIG);
    TCHECK_EQ(csmb_sched_subscribe(&sc, 1, 99, 0, 1, 0), CSMB_EBADTYPE);
    TCHECK_EQ(csmb_sched_unsubscribe(&sc, a), CSMB_OK);
    TCHECK_EQ(csmb_sched_unsubscribe(&sc, a), CSMB_ENOSPAN);
    csmb_sched_free(&sc);
}

/* ---- overlapping spans ---- */

/* The field case that used to be refused with CSMB_EOVERLAP: two
 * 32-bit values at ACTL holding 1023 and 1024 share register 1024
 * (spans [1023,1025) and [1024,1026)).  Both must be polled via one
 * merged read and get their own values / change detection. */
static void test_overlapping_spans(void)
{
    csmb_sched sc;
    csmb_engine e;
    csmb_pending p;
    int32_t a, b;
    uint16_t v0[3] = { 11, 22, 33 };
    uint16_t v1[3] = { 11, 55, 33 };   /* only the shared register changes */
    uint16_t v2[3] = { 11, 55, 44 };   /* only span B's register changes */
    struct evrec *ev;
    int i;

    csmb_sched_init(&sc);
    csmb_engine_init(&e, NULL, NULL, NULL);
    csmb_sched_add_unit(&sc, 1, 0, 20000, 0);
    a = csmb_sched_subscribe(&sc, 1, CSMB_HOLDING, 1023, 2, 0);
    b = csmb_sched_subscribe(&sc, 1, CSMB_HOLDING, 1024, 2, 0);
    TCHECK(a > 0 && b > 0);

    /* the poll program merges the two spans into one covering read */
    csmb_sched_start_round(&e, &sc);
    TCHECK_EQ(csmb_sched_pick(&sc, &p), CSMB_PICK_READ);
    TCHECK_EQ(p.start, 1023);
    TCHECK_EQ(p.count, 3);
    TCHECK_EQ(csmb_sched_pick(&sc, &p), CSMB_PICK_NONE);

    /* first read: both spans online, each publishes its own window */
    one_read(&e, &sc, CSMB_HOLDING, v0, 3);
    capture(&e);
    TCHECK_EQ(count_type(CSMB_EV_SPAN_STATE), 2);
    TCHECK_EQ(count_type(CSMB_EV_SPAN_UPDATE), 2);
    for (i = 0; i < 2; i++) {
        ev = nth_type(CSMB_EV_SPAN_UPDATE, i);
        TCHECK(ev != NULL);
        if (ev->span_id == a) {
            TCHECK_EQ(ev->start, 1023);
            TCHECK_EQ(ev->vals[0], 11);
            TCHECK_EQ(ev->vals[1], 22);
        } else {
            TCHECK_EQ(ev->span_id, b);
            TCHECK_EQ(ev->start, 1024);
            TCHECK_EQ(ev->vals[0], 22);
            TCHECK_EQ(ev->vals[1], 33);
        }
    }

    /* unchanged -> nothing */
    one_read(&e, &sc, CSMB_HOLDING, v0, 3);
    capture(&e);
    TCHECK_EQ(ncap, 0);

    /* the shared register changes -> both spans update */
    one_read(&e, &sc, CSMB_HOLDING, v1, 3);
    capture(&e);
    TCHECK_EQ(count_type(CSMB_EV_SPAN_UPDATE), 2);

    /* a register covered only by span B changes -> only B updates */
    one_read(&e, &sc, CSMB_HOLDING, v2, 3);
    capture(&e);
    TCHECK_EQ(count_type(CSMB_EV_SPAN_UPDATE), 1);
    ev = nth_type(CSMB_EV_SPAN_UPDATE, 0);
    TCHECK(ev != NULL);
    TCHECK_EQ(ev->span_id, b);
    TCHECK_EQ(ev->vals[1], 44);

    /* unsubscribing one span keeps the other working */
    TCHECK_EQ(csmb_sched_unsubscribe(&sc, a), CSMB_OK);
    csmb_sched_start_round(&e, &sc);
    TCHECK_EQ(csmb_sched_pick(&sc, &p), CSMB_PICK_READ);
    TCHECK_EQ(p.start, 1024);
    TCHECK_EQ(p.count, 2);

    csmb_sched_free(&sc);
    csmb_engine_free(&e);
}

/* ---- change detection ---- */

static void test_change_detection(void)
{
    csmb_sched sc;
    csmb_engine e;
    csmb_pending p;
    int32_t s;
    uint16_t a[3] = { 100, 101, 102 };
    uint16_t b[3] = { 100, 999, 102 };
    struct evrec *ev;

    csmb_sched_init(&sc);
    csmb_engine_init(&e, NULL, NULL, NULL);
    csmb_sched_add_unit(&sc, 1, 0, 20000, 0);
    s = csmb_sched_subscribe(&sc, 1, CSMB_HOLDING, 10, 3, 0);
    TCHECK(s > 0);

    /* first read: unit online, span online, first value published */
    one_read(&e, &sc, CSMB_HOLDING, a, 3);
    capture(&e);
    TCHECK_EQ(count_type(CSMB_EV_UNIT_STATE), 1);
    TCHECK_EQ(nth_type(CSMB_EV_UNIT_STATE, 0)->state, CSMB_ST_ONLINE);
    TCHECK_EQ(count_type(CSMB_EV_SPAN_STATE), 1);
    TCHECK_EQ(nth_type(CSMB_EV_SPAN_STATE, 0)->state, CSMB_ST_ONLINE);
    ev = nth_type(CSMB_EV_SPAN_UPDATE, 0);
    TCHECK(ev != NULL);
    TCHECK_EQ(ev->span_id, s);
    TCHECK_EQ(ev->start, 10);
    TCHECK_EQ(ev->count, 3);
    TCHECK_EQ(ev->vals[1], 101);

    /* round exhausted */
    csmb_sched_start_round(&e, &sc);
    TCHECK_EQ(csmb_sched_pick(&sc, &p), CSMB_PICK_READ);
    TCHECK_EQ(csmb_sched_pick(&sc, &p), CSMB_PICK_NONE);

    /* unchanged -> nothing */
    one_read(&e, &sc, CSMB_HOLDING, a, 3);
    capture(&e);
    TCHECK_EQ(ncap, 0);

    /* changed -> update only */
    one_read(&e, &sc, CSMB_HOLDING, b, 3);
    capture(&e);
    TCHECK_EQ(ncap, 1);
    ev = nth_type(CSMB_EV_SPAN_UPDATE, 0);
    TCHECK(ev != NULL);
    TCHECK_EQ(ev->vals[1], 999);

    /* refresh forces a republish of the unchanged value */
    TCHECK_EQ(csmb_sched_refresh_span(&sc, s), CSMB_OK);
    one_read(&e, &sc, CSMB_HOLDING, b, 3);
    capture(&e);
    TCHECK_EQ(count_type(CSMB_EV_SPAN_UPDATE), 1);

    csmb_sched_free(&sc);
    csmb_engine_free(&e);
}

static void test_always_and_coils(void)
{
    csmb_sched sc;
    csmb_engine e;
    uint16_t w[2] = { 5, 6 };
    uint16_t bits[4] = { 1, 0, 1, 1 };
    struct evrec *ev;

    csmb_sched_init(&sc);
    csmb_engine_init(&e, NULL, NULL, NULL);
    csmb_sched_add_unit(&sc, 1, 0, 20000, 0);
    csmb_sched_subscribe(&sc, 1, CSMB_HOLDING, 10, 2, CSMB_SPAN_ALWAYS);

    one_read(&e, &sc, CSMB_HOLDING, w, 2);
    capture(&e);
    TCHECK_EQ(count_type(CSMB_EV_SPAN_UPDATE), 1);
    /* ALWAYS: same value still republishes */
    one_read(&e, &sc, CSMB_HOLDING, w, 2);
    capture(&e);
    TCHECK_EQ(count_type(CSMB_EV_SPAN_UPDATE), 1);
    TCHECK_EQ(count_type(CSMB_EV_UNIT_STATE), 0);   /* already online */

    /* coil read: bits unpacked into per-coil values */
    csmb_sched_subscribe(&sc, 1, CSMB_COIL, 0, 4, 0);
    one_read(&e, &sc, CSMB_COIL, bits, 4);
    capture(&e);
    ev = nth_type(CSMB_EV_SPAN_UPDATE, 0);
    TCHECK(ev != NULL);
    TCHECK_EQ(ev->reg_type, CSMB_COIL);
    TCHECK_EQ(ev->vals[0], 1);
    TCHECK_EQ(ev->vals[1], 0);
    TCHECK_EQ(ev->vals[2], 1);
    TCHECK_EQ(ev->vals[3], 1);

    csmb_sched_free(&sc);
    csmb_engine_free(&e);
}

/* ---- write queue ---- */

static csmb_write_spec wspec(int rt, uint16_t start, uint16_t count,
                             const uint16_t *values)
{
    csmb_write_spec s;

    s.reg_type = (uint8_t)rt;
    s.start = start;
    s.count = count;
    s.values = values;
    return s;
}

static void test_write_ok(void)
{
    csmb_sched sc;
    csmb_engine e;
    csmb_pending p;
    csmb_response r;
    csmb_write_spec specs[2];
    uint16_t one[1] = { 4242 };
    uint16_t three[3] = { 1, 2, 3 };
    uint16_t a[1] = { 100 }, b[1] = { 200 };
    int64_t op;
    struct evrec *ev;

    csmb_sched_init(&sc);
    csmb_engine_init(&e, NULL, NULL, NULL);
    csmb_sched_add_unit(&sc, 1, 0, 20000, 0);

    /* single holding write -> FC6 */
    specs[0] = wspec(CSMB_HOLDING, 5, 1, one);
    op = csmb_sched_enqueue_write(&sc, 1, specs, 1);
    TCHECK(op > 0);
    TCHECK_EQ(csmb_sched_pick(&sc, &p), CSMB_PICK_WRITE);
    TCHECK_EQ(p.fc, CSMB_FC_WRITE_SINGLE_REG);
    TCHECK_EQ(p.op_id, op);
    r = resp_echo(CSMB_FC_WRITE_SINGLE_REG, 5, 4242);
    csmb_sched_on_response(&e, &sc, &p, &r, 0);
    capture(&e);
    ev = nth_type(CSMB_EV_WRITE_DONE, 0);
    TCHECK(ev != NULL);
    TCHECK_EQ(ev->op_id, op);
    TCHECK_EQ(ev->state, CSMB_WR_OK);
    TCHECK_EQ(csmb_sched_pick(&sc, &p), CSMB_PICK_NONE);

    /* multi holding write -> FC16 */
    specs[0] = wspec(CSMB_HOLDING, 10, 3, three);
    op = csmb_sched_enqueue_write(&sc, 1, specs, 1);
    TCHECK_EQ(csmb_sched_pick(&sc, &p), CSMB_PICK_WRITE);
    TCHECK_EQ(p.fc, CSMB_FC_WRITE_MULTI_REGS);
    r = resp_echo(CSMB_FC_WRITE_MULTI_REGS, 10, 3);
    csmb_sched_on_response(&e, &sc, &p, &r, 0);
    capture(&e);
    TCHECK_EQ(nth_type(CSMB_EV_WRITE_DONE, 0)->state, CSMB_WR_OK);

    /* two-request op: WRITE_DONE only after both echo */
    specs[0] = wspec(CSMB_HOLDING, 5, 1, a);
    specs[1] = wspec(CSMB_HOLDING, 6, 1, b);
    op = csmb_sched_enqueue_write(&sc, 1, specs, 2);
    TCHECK_EQ(csmb_sched_pick(&sc, &p), CSMB_PICK_WRITE);
    TCHECK_EQ(p.req_index, 0);
    r = resp_echo(CSMB_FC_WRITE_SINGLE_REG, 5, 100);
    csmb_sched_on_response(&e, &sc, &p, &r, 0);
    capture(&e);
    TCHECK_EQ(count_type(CSMB_EV_WRITE_DONE), 0);
    TCHECK_EQ(csmb_sched_pick(&sc, &p), CSMB_PICK_WRITE);
    TCHECK_EQ(p.req_index, 1);
    r = resp_echo(CSMB_FC_WRITE_SINGLE_REG, 6, 200);
    csmb_sched_on_response(&e, &sc, &p, &r, 0);
    capture(&e);
    TCHECK_EQ(nth_type(CSMB_EV_WRITE_DONE, 0)->state, CSMB_WR_OK);

    csmb_sched_free(&sc);
    csmb_engine_free(&e);
}

static void test_write_errors(void)
{
    csmb_sched sc;
    csmb_engine e;
    csmb_pending p;
    csmb_response r;
    csmb_write_spec specs[1];
    uint16_t seven[1] = { 7 };
    struct evrec *ev;

    csmb_sched_init(&sc);
    csmb_engine_init(&e, NULL, NULL, NULL);
    csmb_sched_add_unit(&sc, 1, 0, 20000, 0);
    csmb_sched_add_unit(&sc, 2, 0, 20000, CSMB_UNIT_NO_VERIFY_WRITE);
    csmb_sched_add_unit(&sc, 3, 0, 20000, CSMB_UNIT_FC1516_ONLY);

    /* exception response fails the op */
    specs[0] = wspec(CSMB_HOLDING, 5, 1, seven);
    csmb_sched_enqueue_write(&sc, 1, specs, 1);
    TCHECK_EQ(csmb_sched_pick(&sc, &p), CSMB_PICK_WRITE);
    r = resp_exc(CSMB_FC_WRITE_SINGLE_REG, CSMB_EXC_ILLEGAL_ADDRESS);
    csmb_sched_on_response(&e, &sc, &p, &r, 0);
    capture(&e);
    ev = nth_type(CSMB_EV_WRITE_DONE, 0);
    TCHECK(ev != NULL);
    TCHECK_EQ(ev->state, CSMB_WR_EXCEPTION);
    TCHECK_EQ(ev->exception, CSMB_EXC_ILLEGAL_ADDRESS);

    /* wrong echo -> verify failed */
    csmb_sched_enqueue_write(&sc, 1, specs, 1);
    TCHECK_EQ(csmb_sched_pick(&sc, &p), CSMB_PICK_WRITE);
    r = resp_echo(CSMB_FC_WRITE_SINGLE_REG, 5, 999);
    csmb_sched_on_response(&e, &sc, &p, &r, 0);
    capture(&e);
    TCHECK_EQ(nth_type(CSMB_EV_WRITE_DONE, 0)->state, CSMB_WR_VERIFY_FAILED);

    /* NO_VERIFY_WRITE accepts the wrong echo */
    csmb_sched_enqueue_write(&sc, 2, specs, 1);
    TCHECK_EQ(csmb_sched_pick(&sc, &p), CSMB_PICK_WRITE);
    r = resp_echo(CSMB_FC_WRITE_SINGLE_REG, 5, 999);
    csmb_sched_on_response(&e, &sc, &p, &r, 0);
    capture(&e);
    TCHECK_EQ(nth_type(CSMB_EV_WRITE_DONE, 0)->state, CSMB_WR_OK);

    /* FC1516_ONLY forces a count-1 write onto FC16 */
    csmb_sched_enqueue_write(&sc, 3, specs, 1);
    TCHECK_EQ(csmb_sched_pick(&sc, &p), CSMB_PICK_WRITE);
    TCHECK_EQ(p.fc, CSMB_FC_WRITE_MULTI_REGS);
    r = resp_echo(CSMB_FC_WRITE_MULTI_REGS, 5, 1);
    csmb_sched_on_response(&e, &sc, &p, &r, 0);
    capture(&e);
    TCHECK_EQ(nth_type(CSMB_EV_WRITE_DONE, 0)->state, CSMB_WR_OK);

    csmb_sched_free(&sc);
    csmb_engine_free(&e);
}

/* writes preempt reads; a read whose span a pending write overlaps is
 * held (not published) until the write completes */
static void test_write_preempt_and_hold(void)
{
    csmb_sched sc;
    csmb_engine e;
    csmb_pending pr, pw;
    csmb_response r;
    csmb_write_spec specs[1];
    uint16_t two[2] = { 700, 701 };
    uint16_t first[2] = { 70, 71 };
    uint16_t changed[2] = { 80, 81 };

    csmb_sched_init(&sc);
    csmb_engine_init(&e, NULL, NULL, NULL);
    csmb_sched_add_unit(&sc, 1, 0, 20000, 0);
    csmb_sched_subscribe(&sc, 1, CSMB_HOLDING, 30, 2, 0);

    /* establish an online value */
    one_read(&e, &sc, CSMB_HOLDING, first, 2);
    capture(&e);

    /* preemption: a queued write is picked before the pending read */
    specs[0] = wspec(CSMB_HOLDING, 30, 2, two);
    csmb_sched_enqueue_write(&sc, 1, specs, 1);
    csmb_sched_start_round(&e, &sc);
    TCHECK_EQ(csmb_sched_pick(&sc, &pr), CSMB_PICK_WRITE);   /* not READ */
    (void)pr;

    /* now drive the hold path: a read is in flight, overlapping write queued;
       its response must not publish */
    csmb_sched_start_round(&e, &sc);   /* rebuild; write still pending */
    /* first pick is still the write (preempts) -> complete it */
    TCHECK_EQ(csmb_sched_pick(&sc, &pw), CSMB_PICK_WRITE);
    r = resp_echo(CSMB_FC_WRITE_MULTI_REGS, 30, 2);
    csmb_sched_on_response(&e, &sc, &pw, &r, 0);
    capture(&e);
    TCHECK_EQ(nth_type(CSMB_EV_WRITE_DONE, 0)->state, CSMB_WR_OK);

    /* simulate the classic race directly: pick a read, THEN queue an
       overlapping write, THEN deliver the read response */
    csmb_sched_start_round(&e, &sc);
    TCHECK_EQ(csmb_sched_pick(&sc, &pr), CSMB_PICK_READ);
    specs[0] = wspec(CSMB_HOLDING, 30, 2, two);
    csmb_sched_enqueue_write(&sc, 1, specs, 1);
    r = resp_read(CSMB_HOLDING, changed, 2);
    csmb_sched_on_response(&e, &sc, &pr, &r, 0);   /* held: no span update */
    capture(&e);
    TCHECK_EQ(count_type(CSMB_EV_SPAN_UPDATE), 0);

    csmb_sched_free(&sc);
    csmb_engine_free(&e);
}

/* ---- poll-seq coverage / :uncovered ---- */

static void test_poll_seq_uncovered(void)
{
    csmb_sched sc;
    csmb_engine e;
    csmb_pending p;
    csmb_response r;
    csmb_range seq[1];
    int32_t s1, s2;
    uint16_t five[5] = { 50, 51, 52, 53, 54 };
    struct evrec *ev;

    csmb_sched_init(&sc);
    csmb_engine_init(&e, NULL, NULL, NULL);
    csmb_sched_add_unit(&sc, 1, 0, 20000, 0);
    s1 = csmb_sched_subscribe(&sc, 1, CSMB_HOLDING, 10, 3, 0);
    s2 = csmb_sched_subscribe(&sc, 1, CSMB_HOLDING, 100, 2, 0);
    TCHECK(s1 > 0 && s2 > 0);

    seq[0].start = 10; seq[0].count = 5;
    TCHECK_EQ(csmb_sched_set_poll_seq(&sc, 1, CSMB_HOLDING, seq, 1), CSMB_OK);

    /* build: s2 is not covered -> one-shot UNCOVERED */
    csmb_sched_start_round(&e, &sc);
    capture(&e);
    TCHECK_EQ(count_type(CSMB_EV_SPAN_STATE), 1);
    ev = nth_type(CSMB_EV_SPAN_STATE, 0);
    TCHECK_EQ(ev->span_id, s2);
    TCHECK_EQ(ev->state, CSMB_ST_UNCOVERED);

    /* the poll-seq range is read; s1 publishes, s2 does not */
    TCHECK_EQ(csmb_sched_pick(&sc, &p), CSMB_PICK_READ);
    TCHECK_EQ(p.start, 10);
    TCHECK_EQ(p.count, 5);
    r = resp_read(CSMB_HOLDING, five, 5);
    csmb_sched_on_response(&e, &sc, &p, &r, 0);
    capture(&e);
    ev = nth_type(CSMB_EV_SPAN_UPDATE, 0);
    TCHECK(ev != NULL);
    TCHECK_EQ(ev->span_id, s1);
    TCHECK_EQ(ev->vals[0], 50);
    TCHECK_EQ(csmb_sched_pick(&sc, &p), CSMB_PICK_NONE);

    /* second round: UNCOVERED not re-emitted */
    csmb_sched_start_round(&e, &sc);
    capture(&e);
    TCHECK_EQ(count_type(CSMB_EV_SPAN_STATE), 0);

    /* clearing the poll-seq covers s2 again (auto-bunch) */
    TCHECK_EQ(csmb_sched_set_poll_seq(&sc, 1, CSMB_HOLDING, NULL, 0), CSMB_OK);
    csmb_sched_start_round(&e, &sc);
    capture(&e);
    TCHECK_EQ(count_type(CSMB_EV_SPAN_STATE), 0);
    /* both spans are now separate read steps */
    TCHECK_EQ(csmb_sched_pick(&sc, &p), CSMB_PICK_READ);
    TCHECK_EQ(p.start, 10);
    TCHECK_EQ(csmb_sched_pick(&sc, &p), CSMB_PICK_READ);
    TCHECK_EQ(p.start, 100);

    csmb_sched_free(&sc);
    csmb_engine_free(&e);
}

/* ---- enable / disable ---- */

static void test_enable_disable(void)
{
    csmb_sched sc;
    csmb_engine e;
    csmb_pending p;
    uint16_t w[2] = { 5, 6 };

    csmb_sched_init(&sc);
    csmb_engine_init(&e, NULL, NULL, NULL);
    csmb_sched_add_unit(&sc, 1, 0, 20000, 0);
    csmb_sched_subscribe(&sc, 1, CSMB_HOLDING, 10, 2, 0);

    one_read(&e, &sc, CSMB_HOLDING, w, 2);
    capture(&e);

    /* disable: unit offline + span stale, and no read steps */
    TCHECK_EQ(csmb_sched_set_unit_enabled(&e, &sc, 1, 0), CSMB_OK);
    capture(&e);
    TCHECK_EQ(nth_type(CSMB_EV_UNIT_STATE, 0)->state, CSMB_ST_OFFLINE);
    TCHECK_EQ(nth_type(CSMB_EV_SPAN_STATE, 0)->state, CSMB_ST_STALE);
    csmb_sched_start_round(&e, &sc);
    TCHECK_EQ(csmb_sched_pick(&sc, &p), CSMB_PICK_NONE);

    /* re-enable + read: republishes the (unchanged) value */
    TCHECK_EQ(csmb_sched_set_unit_enabled(&e, &sc, 1, 1), CSMB_OK);
    capture(&e);
    TCHECK_EQ(ncap, 0);
    one_read(&e, &sc, CSMB_HOLDING, w, 2);
    capture(&e);
    TCHECK_EQ(count_type(CSMB_EV_UNIT_STATE), 1);
    TCHECK_EQ(nth_type(CSMB_EV_UNIT_STATE, 0)->state, CSMB_ST_ONLINE);
    TCHECK_EQ(count_type(CSMB_EV_SPAN_UPDATE), 1);

    TCHECK_EQ(csmb_sched_set_unit_enabled(&e, &sc, 9, 0), CSMB_ENOUNIT);

    csmb_sched_free(&sc);
    csmb_engine_free(&e);
}

/* did any captured SPAN_STATE for span ID carry STATE? */
static int had_span_state(int32_t id, uint8_t state)
{
    int i;

    for (i = 0; i < ncap; i++)
        if (cap[i].type == CSMB_EV_SPAN_STATE && cap[i].span_id == id &&
            cap[i].state == state)
            return 1;
    return 0;
}

/* did any captured UNIT_STATE carry STATE? */
static int had_unit_state(uint8_t state)
{
    int i;

    for (i = 0; i < ncap; i++)
        if (cap[i].type == CSMB_EV_UNIT_STATE && cap[i].state == state)
            return 1;
    return 0;
}

/* ---- staleness (sweep) & connection down/up ---- */

static void test_stale_and_connection(void)
{
    csmb_sched sc;
    csmb_engine e;
    csmb_pending p;
    csmb_response r;
    csmb_write_spec specs[1];
    uint16_t w[2] = { 5, 6 };
    uint16_t nine[1] = { 9 };
    struct evrec *ev;
    csmb_usec_t t = 1000000;   /* 1s; the unit's stale-timeout is 1s */

    csmb_sched_init(&sc);
    csmb_engine_init(&e, NULL, NULL, NULL);
    csmb_sched_add_unit(&sc, 1, 0, 1000, 0);
    csmb_sched_subscribe(&sc, 1, CSMB_HOLDING, 10, 2, 0);

    /* first read: online + value */
    csmb_sched_start_round(&e, &sc);
    TCHECK_EQ(csmb_sched_pick(&sc, &p), CSMB_PICK_READ);
    r = resp_read(CSMB_HOLDING, w, 2);
    csmb_sched_on_response(&e, &sc, &p, &r, t);
    capture(&e);
    TCHECK_EQ(nth_type(CSMB_EV_UNIT_STATE, 0)->state, CSMB_ST_ONLINE);

    /* sweep before the timeout: nothing */
    csmb_sched_staleness_sweep(&e, &sc, t + 500000);
    capture(&e);
    TCHECK_EQ(ncap, 0);

    /* sweep past the timeout: unit offline + span stale */
    csmb_sched_staleness_sweep(&e, &sc, t + 2000000);
    capture(&e);
    TCHECK_EQ(count_type(CSMB_EV_UNIT_STATE), 1);
    TCHECK_EQ(nth_type(CSMB_EV_UNIT_STATE, 0)->state, CSMB_ST_OFFLINE);
    TCHECK_EQ(count_type(CSMB_EV_SPAN_STATE), 1);
    TCHECK_EQ(nth_type(CSMB_EV_SPAN_STATE, 0)->state, CSMB_ST_STALE);

    /* recovery republishes */
    csmb_sched_start_round(&e, &sc);
    TCHECK_EQ(csmb_sched_pick(&sc, &p), CSMB_PICK_READ);
    r = resp_read(CSMB_HOLDING, w, 2);
    csmb_sched_on_response(&e, &sc, &p, &r, t + 3000000);
    capture(&e);
    TCHECK_EQ(nth_type(CSMB_EV_UNIT_STATE, 0)->state, CSMB_ST_ONLINE);
    TCHECK_EQ(nth_type(CSMB_EV_SPAN_STATE, 0)->state, CSMB_ST_ONLINE);
    TCHECK_EQ(count_type(CSMB_EV_SPAN_UPDATE), 1);

    /* connection down: queued write fails, unit + span go offline */
    specs[0] = wspec(CSMB_HOLDING, 10, 1, nine);
    csmb_sched_enqueue_write(&sc, 1, specs, 1);
    csmb_sched_connection_down(&e, &sc);
    capture(&e);
    ev = nth_type(CSMB_EV_WRITE_DONE, 0);
    TCHECK(ev != NULL);
    TCHECK_EQ(ev->state, CSMB_WR_CONN_FAILED);
    TCHECK_EQ(nth_type(CSMB_EV_UNIT_STATE, 0)->state, CSMB_ST_OFFLINE);
    TCHECK_EQ(nth_type(CSMB_EV_SPAN_STATE, 0)->state, CSMB_ST_OFFLINE);

    /* connection up clears values; next read republishes */
    csmb_sched_connection_up(&sc);
    capture(&e);
    TCHECK_EQ(ncap, 0);
    csmb_sched_start_round(&e, &sc);
    TCHECK_EQ(csmb_sched_pick(&sc, &p), CSMB_PICK_READ);
    r = resp_read(CSMB_HOLDING, w, 2);
    csmb_sched_on_response(&e, &sc, &p, &r, t + 4000000);
    capture(&e);
    TCHECK_EQ(count_type(CSMB_EV_SPAN_UPDATE), 1);
    TCHECK_EQ(csmb_sched_pick(&sc, &p), CSMB_PICK_NONE);

    csmb_sched_free(&sc);
    csmb_engine_free(&e);
}

/* Delayed exception staleness: a span that always draws exceptions stales
 * only after the stale-timeout (not immediately), a sibling span that
 * still reads fine stays online, and the unit stays online because it
 * keeps answering (with exceptions). */
static void test_exception_sibling(void)
{
    csmb_sched sc;
    csmb_engine e;
    csmb_pending p;
    csmb_response r;
    int32_t good, bad;
    uint16_t w[2] = { 5, 6 };
    csmb_usec_t t;

    csmb_sched_init(&sc);
    csmb_engine_init(&e, NULL, NULL, NULL);
    csmb_sched_add_unit(&sc, 1, 0, 1000, 0);   /* 1s stale-timeout */
    good = csmb_sched_subscribe(&sc, 1, CSMB_HOLDING, 10, 2, 0);
    bad  = csmb_sched_subscribe(&sc, 1, CSMB_HOLDING, 100, 2, 0);
    TCHECK(good > 0 && bad > 0);

    /* poll every 200ms from 1s to 3s: good reads OK, bad always excepts */
    for (t = 1000000; t <= 3000000; t += 200000) {
        csmb_sched_start_round(&e, &sc);
        while (csmb_sched_pick(&sc, &p) == CSMB_PICK_READ) {
            if (p.start == 10)
                r = resp_read(CSMB_HOLDING, w, 2);
            else
                r = resp_exc(rd_fc(CSMB_HOLDING), CSMB_EXC_ILLEGAL_ADDRESS);
            csmb_sched_on_response(&e, &sc, &p, &r, t);
        }
        csmb_sched_staleness_sweep(&e, &sc, t);
    }
    capture(&e);   /* the whole run's events */

    TCHECK(had_span_state(good, CSMB_ST_ONLINE));   /* good came online */
    TCHECK(!had_span_state(good, CSMB_ST_STALE));    /* and stayed online */
    TCHECK(had_span_state(bad, CSMB_ST_STALE));       /* bad eventually staled */
    TCHECK(!had_unit_state(CSMB_ST_OFFLINE));         /* unit never went offline */
    TCHECK(count_type(CSMB_EV_LOG) >= 1);             /* an exception was logged */

    csmb_sched_free(&sc);
    csmb_engine_free(&e);
}

TEST_MAIN(test_bunch, test_subscribe, test_overlapping_spans,
          test_change_detection,
          test_always_and_coils, test_write_ok, test_write_errors,
          test_write_preempt_and_hold, test_poll_seq_uncovered,
          test_enable_disable, test_stale_and_connection, test_exception_sibling)
