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

#endif /* CSMB_PRIVATE_H_INCLUDED */
