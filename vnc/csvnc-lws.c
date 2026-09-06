/* The lws-facing server: a listener vhost, per-client protocol state
 * and the framebuffer-update pump.
 *
 * Each client owns ONE fixed-size output buffer.  Handshake replies
 * are queued into it as the parser produces them; framebuffer updates
 * are generated into it one band (a rectangle at most band_rows tall)
 * per RAW_WRITEABLE once it has drained, so a client that never reads
 * costs its buffer plus a bounded dirty region and nothing more.  An
 * update answers one FramebufferUpdateRequest: it snapshots the
 * client's dirty region at start (later blits accumulate for the next
 * request) and streams the snapshot in bands from the live shadow
 * under the exact band count.  The LastRect pseudo-encoding is never
 * used even when offered: vncsnapshot offers it and then treats the
 * terminating 0x0 rectangle as junk and waits forever.  Protocol
 * violations close that client only. */

#include <libwebsockets.h>
#include "csvnc-private.h"

#define CSVNC_PROTOCOL_NAME "cs-vnc"
#define CSVNC_BAND_PIXELS   (640 * 64)   /* per-band encode budget */

typedef struct csvnc_client {
    struct csvnc_client *next;
    csvnc_server *srv;          /* NULL once the server is destroyed */
    struct lws *wsi;
    csvnc_rfb rfb;
    csvnc_region pending;       /* dirty since the last update started */
    csvnc_region inflight;      /* the update being streamed */
    csvnc_pixfmt fmt;           /* format the current update uses */
    csvnc_encoder enc;
    int update_requested;
    int in_update;
    uint8_t *buf;               /* LWS_PRE + cap bytes */
    size_t cap, len, off;       /* queued bytes, bytes already written */
} csvnc_client;

struct csvnc_server {
    struct csvnc_server *next_server;   /* process-wide registry */
    struct lws_context *cx;
    struct lws_vhost *vhost;
    char *vhost_name, *role_str, *proto_str, *iface_str;
    csvnc_fb fb;
    csvnc_callbacks cbs;
    void *user;
    csvnc_client *clients;
    int band_rows;
    size_t client_cap;          /* output buffer size per client */
};

/* lws shares one listen socket between vhosts on the same port of a
 * context, so a second server on a port we already serve would not
 * fail to bind: it would silently steal or share connections.  A
 * registry of live servers refuses that up front. */
static csvnc_server *servers;

static int port_in_use(int port)
{
    const csvnc_server *s;

    for (s = servers; s; s = s->next_server)
        if (port && csvnc_listen_port(s) == port)
            return 1;
    return 0;
}

static void unregister_server(csvnc_server *s)
{
    csvnc_server **pp;

    for (pp = &servers; *pp; pp = &(*pp)->next_server) {
        if (*pp == s) {
            *pp = s->next_server;
            return;
        }
    }
}

/* ---- output buffer ---- */

static int client_drained(const csvnc_client *c)
{
    return c->off >= c->len;
}

static void client_queue(csvnc_client *c, const uint8_t *data, size_t n)
{
    if (client_drained(c))
        c->len = c->off = 0;
    if (c->len + n > c->cap) {
        /* cannot happen: handshake output is tiny and updates are
         * generated only into a drained buffer; drop the client */
        lwsl_err("csvnc: client output buffer overflow\n");
        lws_set_timeout(c->wsi, PENDING_TIMEOUT_HTTP_CONTENT, LWS_TO_KILL_ASYNC);
        return;
    }
    memcpy(c->buf + LWS_PRE + c->len, data, n);
    c->len += n;
    lws_callback_on_writable(c->wsi);
}

/* An update can start when the client asked for one, something is
 * dirty, nothing is in flight and the buffer is empty. */
static int client_update_ready(const csvnc_client *c)
{
    return csvnc_rfb_established(&c->rfb) && c->update_requested
        && !c->in_update && !csvnc_region_empty(&c->pending)
        && client_drained(c);
}

static void client_kick(csvnc_client *c)
{
    if (client_update_ready(c))
        lws_callback_on_writable(c->wsi);
}

/* ---- update generation ---- */

static unsigned count_bands(const csvnc_region *rg, int band_rows)
{
    unsigned n = 0;
    int i;

    for (i = 0; i < rg->n; i++)
        n += (unsigned)((rg->r[i].h + band_rows - 1) / band_rows);
    return n;
}

static void client_start_update(csvnc_client *c)
{
    uint8_t hdr[CSVNC_UPDATE_HDR_SIZE];

    c->inflight = c->pending;
    csvnc_region_clear(&c->pending);
    c->fmt = c->rfb.fmt;
    csvnc_encoder_select(&c->enc, c->rfb.enc);
    c->in_update = 1;
    c->update_requested = 0;
    csvnc_put_update_header(hdr, count_bands(&c->inflight, c->srv->band_rows));
    c->len = c->off = 0;
    memcpy(c->buf + LWS_PRE, hdr, sizeof(hdr));
    c->len = sizeof(hdr);
}

/* Generate the next piece of the in-flight update into the (drained)
 * buffer.  Returns -1 when encoding fails (cannot happen with the
 * buffer sized by csvnc_encode_max_size). */
static int client_continue_update(csvnc_client *c)
{
    csvnc_rect r;
    size_t n;

    if (client_drained(c))
        c->len = c->off = 0;
    if (csvnc_region_pop(&c->inflight, c->srv->band_rows, &r)) {
        n = csvnc_encode_rect(&c->enc, &c->fmt, &c->srv->fb, r.x, r.y, r.w, r.h,
                              c->buf + LWS_PRE + c->len, c->cap - c->len);
        if (!n) {
            lwsl_err("csvnc: %dx%d band does not fit the client buffer\n",
                     r.w, r.h);
            return -1;
        }
        c->len += n;
    }
    if (csvnc_region_empty(&c->inflight))
        c->in_update = 0;           /* the last band is queued */
    return 0;
}

static int client_writable(csvnc_client *c)
{
    int written;

    if (client_drained(c)) {
        if (c->in_update) {
            if (client_continue_update(c) < 0)
                return -1;
        } else if (client_update_ready(c)) {
            client_start_update(c);
        } else {
            return 0;
        }
    }
    written = lws_write(c->wsi, c->buf + LWS_PRE + c->off, c->len - c->off,
                        LWS_WRITE_RAW);
    if (written < 0)
        return -1;
    c->off += (size_t)written;
    if (!client_drained(c) || c->in_update || client_update_ready(c))
        lws_callback_on_writable(c->wsi);
    return 0;
}

/* ---- rfb ops ---- */

static void op_send(void *user, const uint8_t *data, size_t len)
{
    client_queue(user, data, len);
}

static void op_pixel_format(void *user)
{
    (void)user;   /* picked up by the next update */
}

static void op_encodings(void *user)
{
    (void)user;   /* picked up by the next update */
}

static void op_update_request(void *user, int incremental,
                              int x, int y, int w, int h)
{
    csvnc_client *c = user;

    if (!incremental)
        csvnc_region_add(&c->pending, x, y, w, h);
    c->update_requested = 1;
    client_kick(c);
}

static void op_key(void *user, uint32_t keysym, int down)
{
    csvnc_client *c = user;
    int32_t sdl = csvnc_keysym_to_sdl(keysym);

    if (sdl && c->srv && c->srv->cbs.key)
        c->srv->cbs.key(c->srv->user, sdl, down);
}

static void op_pointer(void *user, int x, int y, unsigned buttons)
{
    csvnc_client *c = user;

    if (c->srv && c->srv->cbs.pointer)
        c->srv->cbs.pointer(c->srv->user, x, y, buttons);
}

static const csvnc_rfb_ops client_ops = {
    op_send, op_pixel_format, op_encodings, op_update_request, op_key,
    op_pointer
};

/* ---- client lifecycle ---- */

static csvnc_client *client_create(csvnc_server *s, struct lws *wsi)
{
    csvnc_client *c = calloc(1, sizeof(*c));

    if (!c)
        return NULL;
    c->cap = s->client_cap;
    c->buf = malloc(LWS_PRE + c->cap);
    if (!c->buf) {
        free(c);
        return NULL;
    }
    c->srv = s;
    c->wsi = wsi;
    csvnc_rfb_init(&c->rfb, &client_ops, c, s->fb.width, s->fb.height,
                   "EACS console");
    csvnc_region_init(&c->pending, s->fb.width, s->fb.height);
    csvnc_region_init(&c->inflight, s->fb.width, s->fb.height);
    csvnc_pixfmt_native(&c->fmt);
    csvnc_encoder_init(&c->enc);
    c->next = s->clients;
    s->clients = c;
    return c;
}

static void client_free(csvnc_client *c)
{
    csvnc_encoder_free(&c->enc);
    free(c->buf);
    free(c);
}

static void server_unlink_client(csvnc_server *s, csvnc_client *c)
{
    csvnc_client **pp;

    for (pp = &s->clients; *pp; pp = &(*pp)->next) {
        if (*pp == c) {
            *pp = c->next;
            return;
        }
    }
}

/* ---- the protocol callback ---- */

int csvnc_lws_protocol_callback(struct lws *wsi, int reason,
                                void *user, void *in, size_t len)
{
    csvnc_client *c;

    (void)user;

    switch ((enum lws_callback_reasons)reason) {
    case LWS_CALLBACK_RAW_ADOPT: {
        struct lws_vhost *vh = lws_get_vhost(wsi);
        csvnc_server *s = vh ? lws_vhost_user(vh) : NULL;

        if (!s)
            return -1;
        c = client_create(s, wsi);
        if (!c)
            return -1;
        lws_set_opaque_user_data(wsi, c);
        csvnc_rfb_start(&c->rfb);
        return 0;
    }

    case LWS_CALLBACK_RAW_RX:
        c = lws_get_opaque_user_data(wsi);
        if (!c || !c->srv)
            return -1;
        if (csvnc_rfb_feed(&c->rfb, in, len) < 0) {
            lwsl_notice("csvnc: dropping client: protocol violation\n");
            return -1;
        }
        return 0;

    case LWS_CALLBACK_RAW_WRITEABLE:
        c = lws_get_opaque_user_data(wsi);
        if (!c || !c->srv)
            return -1;
        return client_writable(c);

    case LWS_CALLBACK_RAW_CLOSE:
        c = lws_get_opaque_user_data(wsi);
        if (!c)
            return 0;
        if (c->srv)
            server_unlink_client(c->srv, c);
        client_free(c);
        lws_set_opaque_user_data(wsi, NULL);
        return 0;

    default:
        return 0;
    }
}

/* ---- public API ---- */

static void server_free(csvnc_server *s)
{
    if (s->vhost)
        lws_vhost_destroy(s->vhost);
    free(s->vhost_name);
    free(s->role_str);
    free(s->proto_str);
    free(s->iface_str);
    csvnc_fb_free(&s->fb);
    free(s);
}

csvnc_server *csvnc_create(struct lws_context *cx,
                           const struct lws_protocols *protocols,
                           const char *iface, int port,
                           int width, int height,
                           const csvnc_callbacks *cbs, void *user)
{
    static unsigned seq;
    struct lws_context_creation_info info;
    csvnc_server *s;
    csvnc_encoder probe;
    csvnc_pixfmt widest;
    char namebuf[48];

    if (!cx || !protocols || port < 0 || port > 65535)
        return NULL;
    if (port_in_use(port)) {
        lwsl_err("csvnc: port %d is already served by another server\n", port);
        return NULL;
    }
    s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    if (csvnc_fb_init(&s->fb, width, height) < 0) {
        free(s);
        return NULL;
    }
    s->cx = cx;
    if (cbs)
        s->cbs = *cbs;
    s->user = user;
    /* bands hold a bounded number of pixels whatever the width */
    s->band_rows = CSVNC_BAND_PIXELS / width;
    if (s->band_rows < 1)
        s->band_rows = 1;
    if (s->band_rows > height)
        s->band_rows = height;
    /* the buffer must hold the update header plus the biggest band in
     * the biggest client format with the costliest encoder */
    csvnc_encoder_init(&probe);
    csvnc_pixfmt_native(&widest);
    s->client_cap = 0;
    {
        static const int32_t encs[] = { CSVNC_ENC_RAW, CSVNC_ENC_HEXTILE,
                                        CSVNC_ENC_ZRLE };
        size_t i, n;

        for (i = 0; i < sizeof(encs) / sizeof(encs[0]); i++) {
            csvnc_encoder_select(&probe, encs[i]);
            n = csvnc_encode_max_size(&probe, &widest, width, s->band_rows);
            if (n > s->client_cap)
                s->client_cap = n;
        }
    }
    s->client_cap += CSVNC_UPDATE_HDR_SIZE;

    snprintf(namebuf, sizeof(namebuf), "csvnc-%u", ++seq);
    s->vhost_name = strdup(namebuf);
    s->role_str = strdup("raw-skt");
    s->proto_str = strdup(CSVNC_PROTOCOL_NAME);
    if (iface)
        s->iface_str = strdup(iface);
    if (!s->vhost_name || !s->role_str || !s->proto_str
        || (iface && !s->iface_str)) {
        server_free(s);
        return NULL;
    }

    memset(&info, 0, sizeof(info));
    info.port = port;
    info.iface = s->iface_str;
    info.protocols = protocols;
    info.options = LWS_SERVER_OPTION_ADOPT_APPLY_LISTEN_ACCEPT_CONFIG;
    info.listen_accept_role = s->role_str;
    info.listen_accept_protocol = s->proto_str;
    info.vhost_name = s->vhost_name;
    info.user = s;
    s->vhost = lws_create_vhost(cx, &info);
    if (!s->vhost) {
        lwsl_err("csvnc: cannot listen on %s:%d\n", iface ? iface : "*", port);
        server_free(s);
        return NULL;
    }
    s->next_server = servers;
    servers = s;
    return s;
}

int csvnc_listen_port(const csvnc_server *s)
{
    return s && s->vhost ? lws_get_vhost_listen_port(s->vhost) : 0;
}

void csvnc_blit(csvnc_server *s, int x, int y, int w, int h,
                const void *px, int stride_bytes, int src_format)
{
    csvnc_region dirty;
    csvnc_client *c;
    int i;

    if (!s)
        return;
    csvnc_region_init(&dirty, s->fb.width, s->fb.height);
    if (!csvnc_fb_blit(&s->fb, x, y, w, h, px, stride_bytes, src_format,
                       &dirty))
        return;
    for (c = s->clients; c; c = c->next) {
        for (i = 0; i < dirty.n; i++)
            csvnc_region_add(&c->pending, dirty.r[i].x, dirty.r[i].y,
                             dirty.r[i].w, dirty.r[i].h);
        client_kick(c);
    }
}

int csvnc_client_count(const csvnc_server *s)
{
    const csvnc_client *c;
    int n = 0;

    if (!s)
        return 0;
    for (c = s->clients; c; c = c->next)
        if (csvnc_rfb_established(&c->rfb))
            n++;
    return n;
}

int csvnc_client_info(const csvnc_server *s, int index,
                      csvnc_client_state *out)
{
    const csvnc_client *c;

    if (!s || index < 0)
        return -1;
    for (c = s->clients; c && index > 0; c = c->next)
        index--;
    if (!c)
        return -1;
    memset(out, 0, sizeof(*out));
    out->established = csvnc_rfb_established(&c->rfb);
    out->pending_rects = c->pending.n;
    out->pending_area = csvnc_region_area(&c->pending);
    out->in_update = c->in_update;
    out->update_requested = c->update_requested;
    out->queued_bytes = c->len - c->off;
    out->encoding = c->rfb.enc;
    out->bpp = c->rfb.fmt.bpp;
    return 0;
}

void csvnc_drop_clients(csvnc_server *s)
{
    csvnc_client *c;

    if (!s)
        return;
    for (c = s->clients; c; c = c->next)
        lws_set_timeout(c->wsi, PENDING_TIMEOUT_HTTP_CONTENT, LWS_TO_KILL_ASYNC);
}

void csvnc_destroy(csvnc_server *s)
{
    csvnc_client *c;

    if (!s)
        return;
    /* detach the clients first: their RAW_CLOSE (which the vhost
     * teardown triggers) then frees them without touching the server */
    for (c = s->clients; c; c = c->next) {
        c->srv = NULL;
        lws_set_timeout(c->wsi, PENDING_TIMEOUT_HTTP_CONTENT, LWS_TO_KILL_ASYNC);
    }
    s->clients = NULL;
    unregister_server(s);
    server_free(s);
}
