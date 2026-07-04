/* Event batching tests: accumulation, payload arena, freeze/drain
 * semantics, notify firing rules, reentrant production during drain. */

#include "../csmb-private.h"
#include "test-util.h"

static csmb_engine test_engine;
static int notify_count;

/* what the last notify observed / did, controlled per test */
static int notify_should_drain;
static int drained_events;

static void test_notify(void *opaque)
{
    csmb_engine *e = opaque;

    notify_count++;
    if (notify_should_drain) {
        const csmb_event *evs;
        size_t n = csmb_events_get(e, &evs);

        drained_events += (int)n;
        csmb_events_done(e);
    }
}

static void add_span_update(csmb_engine *e, int32_t span_id,
                            const uint16_t *payload, size_t n)
{
    CSMB_ZERO_EVENT(ev);
    ev.type = CSMB_EV_SPAN_UPDATE;
    ev.span_id = span_id;
    TCHECK_EQ(csmb_event_add(e, &ev, payload, n), CSMB_OK);
}

static void test_basic_batch(void)
{
    csmb_engine *e = &test_engine;
    const csmb_event *evs;
    uint16_t v1[] = { 1, 2, 3 };
    uint16_t v2[] = { 42 };
    size_t n;

    csmb_engine_init(e, NULL, NULL, NULL);
    add_span_update(e, 7, v1, 3);
    add_span_update(e, 8, v2, 1);
    {
        CSMB_ZERO_EVENT(ev);
        ev.type = CSMB_EV_UNIT_STATE;
        ev.unit = 3;
        ev.state = CSMB_ST_ONLINE;
        TCHECK_EQ(csmb_event_add(e, &ev, NULL, 0), CSMB_OK);
    }

    n = csmb_events_get(e, &evs);
    TCHECK_EQ(n, 3);
    TCHECK_EQ(evs[0].type, CSMB_EV_SPAN_UPDATE);
    TCHECK_EQ(evs[0].span_id, 7);
    TCHECK_EQ(evs[0].count, 3);
    TCHECK(evs[0].values != NULL);
    TCHECK_EQ(evs[0].values[0], 1);
    TCHECK_EQ(evs[0].values[2], 3);
    TCHECK_EQ(evs[1].values[0], 42);
    TCHECK_EQ(evs[2].type, CSMB_EV_UNIT_STATE);
    TCHECK(evs[2].values == NULL);

    /* events added during the drain go to the next batch */
    add_span_update(e, 9, v2, 1);
    TCHECK_EQ(csmb_events_get(e, &evs), 3); /* same frozen batch */
    csmb_events_done(e);

    n = csmb_events_get(e, &evs);
    TCHECK_EQ(n, 1);
    TCHECK_EQ(evs[0].span_id, 9);
    csmb_events_done(e);

    TCHECK_EQ(csmb_events_get(e, &evs), 0);
    csmb_events_done(e);
    csmb_engine_free(e);
}

static void test_arena_growth(void)
{
    csmb_engine *e = &test_engine;
    const csmb_event *evs;
    uint16_t payload[125];
    size_t i, n;

    csmb_engine_init(e, NULL, NULL, NULL);
    for (i = 0; i < 200; i++) {
        size_t j;

        for (j = 0; j < 125; j++)
            payload[j] = (uint16_t)(i + j);
        add_span_update(e, (int32_t)i, payload, 125);
    }
    n = csmb_events_get(e, &evs);
    TCHECK_EQ(n, 200);
    for (i = 0; i < 200; i++) {
        TCHECK_EQ(evs[i].span_id, (int32_t)i);
        TCHECK_EQ(evs[i].count, 125);
        TCHECK_EQ(evs[i].values[0], i);
        TCHECK_EQ(evs[i].values[124], i + 124);
    }
    csmb_events_done(e);
    csmb_engine_free(e);
}

static void test_notify_drains(void)
{
    csmb_engine *e = &test_engine;
    uint16_t v[] = { 5 };

    csmb_engine_init(e, NULL, test_notify, e);
    notify_count = 0;
    drained_events = 0;
    notify_should_drain = 1;

    csmb_event_flush(e); /* empty: no notify */
    TCHECK_EQ(notify_count, 0);

    add_span_update(e, 1, v, 1);
    add_span_update(e, 2, v, 1);
    csmb_event_flush(e);
    TCHECK_EQ(notify_count, 1);
    TCHECK_EQ(drained_events, 2);

    csmb_event_flush(e); /* nothing new */
    TCHECK_EQ(notify_count, 1);
    csmb_engine_free(e);
}

static void test_notify_deferred_drain(void)
{
    csmb_engine *e = &test_engine;
    const csmb_event *evs;
    uint16_t v[] = { 5 };

    /* a consumer that only takes note (wake-once style) must get ONE
     * notify and no spin */
    csmb_engine_init(e, NULL, test_notify, e);
    notify_count = 0;
    notify_should_drain = 0;

    add_span_update(e, 1, v, 1);
    csmb_event_flush(e);
    TCHECK_EQ(notify_count, 1);

    /* later, outside notify, the consumer drains */
    TCHECK_EQ(csmb_events_get(e, &evs), 1);
    csmb_events_done(e);
    csmb_engine_free(e);
}

static int reentrant_adds_left;

static void reentrant_notify(void *opaque)
{
    csmb_engine *e = opaque;
    const csmb_event *evs;
    size_t n = csmb_events_get(e, &evs);
    uint16_t v[] = { 6 };

    notify_count++;
    drained_events += (int)n;
    /* producing from inside the drain: lands in the next batch and
     * re-fires notify from the same flush */
    if (reentrant_adds_left > 0) {
        reentrant_adds_left--;
        add_span_update(e, 100 + reentrant_adds_left, v, 1);
    }
    csmb_events_done(e);
}

static void test_reentrant_production(void)
{
    csmb_engine *e = &test_engine;
    uint16_t v[] = { 5 };

    csmb_engine_init(e, NULL, reentrant_notify, e);
    notify_count = 0;
    drained_events = 0;
    reentrant_adds_left = 2;

    add_span_update(e, 1, v, 1);
    csmb_event_flush(e);
    /* 1 event + 2 reentrant events, drained across 3 notifies */
    TCHECK_EQ(notify_count, 3);
    TCHECK_EQ(drained_events, 3);
    TCHECK_EQ(e->pending.nevents, 0);
    csmb_engine_free(e);
}

TEST_MAIN(test_basic_batch, test_arena_growth, test_notify_drains,
          test_notify_deferred_drain, test_reentrant_production)
