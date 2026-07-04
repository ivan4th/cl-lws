/* Event batching: growable event array + payload arena, frozen-batch
 * handover to the consumer.  See csmb-private.h for the design notes. */

#include "csmb-private.h"

/* While a batch accumulates, csmb_event.values holds the arena offset
 * disguised as a pointer, tagged so that a genuine NULL stays NULL. */
#define CSMB_OFFSET_TAG 1u

static void event_buf_reset(csmb_event_buf *b)
{
    b->nevents = 0;
    b->arena_used = 0;
}

static void event_buf_free(csmb_event_buf *b)
{
    free(b->events);
    free(b->arena);
    memset(b, 0, sizeof(*b));
}

void csmb_engine_init(csmb_engine *e, struct lws_context *cx,
                      csmb_notify_cb notify, void *opaque)
{
    memset(e, 0, sizeof(*e));
    e->cx = cx;
    e->notify = notify;
    e->opaque = opaque;
}

void csmb_engine_free(csmb_engine *e)
{
    event_buf_free(&e->pending);
    event_buf_free(&e->draining);
}

int csmb_event_add(csmb_engine *e, const csmb_event *ev,
                   const uint16_t *payload, size_t n)
{
    csmb_event_buf *b = &e->pending;

    if (b->nevents == b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 64;
        csmb_event *nev = realloc(b->events, ncap * sizeof(*nev));

        if (!nev)
            return CSMB_ENOMEM;
        b->events = nev;
        b->cap = ncap;
    }
    b->events[b->nevents] = *ev;
    if (payload) {
        size_t bytes = n * sizeof(uint16_t);

        if (b->arena_used + bytes > b->arena_cap) {
            size_t ncap = b->arena_cap ? b->arena_cap : 1024;

            while (ncap < b->arena_used + bytes)
                ncap *= 2;
            uint8_t *na = realloc(b->arena, ncap);
            if (!na)
                return CSMB_ENOMEM;
            b->arena = na;
            b->arena_cap = ncap;
        }
        memcpy(b->arena + b->arena_used, payload, bytes);
        b->events[b->nevents].values =
            (const uint16_t *)(uintptr_t)((b->arena_used << 1) | CSMB_OFFSET_TAG);
        b->events[b->nevents].count = (uint16_t)n;
        b->arena_used += bytes;
    } else {
        b->events[b->nevents].values = NULL;
    }
    b->nevents++;
    return CSMB_OK;
}

size_t csmb_events_get(void *master_or_slave, const csmb_event **out)
{
    csmb_engine *e = master_or_slave;
    csmb_event_buf tmp;
    size_t i;

    if (e->drain_active) {
        /* nested get without done: hand out the same frozen batch */
        *out = e->draining.events;
        return e->draining.nevents;
    }
    /* freeze: swap pending and draining buffers (reusing the old
     * draining allocations for the new pending batch) */
    tmp = e->draining;
    e->draining = e->pending;
    e->pending = tmp;
    event_buf_reset(&e->pending);
    e->drain_active = 1;
    e->consumed_in_notify = 1;

    /* fix up arena offsets into real pointers */
    for (i = 0; i < e->draining.nevents; i++) {
        csmb_event *ev = &e->draining.events[i];
        uintptr_t v = (uintptr_t)ev->values;

        if (v & CSMB_OFFSET_TAG)
            ev->values = (const uint16_t *)(e->draining.arena + (v >> 1));
    }
    *out = e->draining.events;
    return e->draining.nevents;
}

void csmb_events_done(void *master_or_slave)
{
    csmb_engine *e = master_or_slave;

    event_buf_reset(&e->draining);
    e->drain_active = 0;
}

void csmb_event_flush(csmb_engine *e)
{
    if (e->destroyed || e->in_notify || !e->notify)
        return;
    /* Re-notify when events were produced from inside a drain (the
     * consumer made engine calls during the notify), but only as long
     * as the consumer keeps draining: a consumer that merely takes
     * note of the notification and drains later must not spin us. */
    while (e->pending.nevents && !e->drain_active) {
        e->consumed_in_notify = 0;
        e->in_notify = 1;
        e->notify(e->opaque);
        e->in_notify = 0;
        if (!e->consumed_in_notify)
            break;
    }
}
