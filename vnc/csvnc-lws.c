/* The lws-facing server: listener vhost, per-client state, write
 * scheduling.  M1 placeholder: the public entry points exist so the
 * library links, but no listener is created yet (csvnc_create fails). */

#include <libwebsockets.h>
#include "csvnc-private.h"

struct csvnc_server {
    struct lws_context *cx;
    csvnc_fb fb;
    csvnc_callbacks cbs;
    void *user;
};

csvnc_server *csvnc_create(struct lws_context *cx, const char *iface,
                           int port, int width, int height,
                           const csvnc_callbacks *cbs, void *user)
{
    (void)cx;
    (void)iface;
    (void)port;
    (void)width;
    (void)height;
    (void)cbs;
    (void)user;
    lwsl_err("csvnc: server not implemented yet\n");
    return NULL;
}

int csvnc_listen_port(const csvnc_server *s)
{
    (void)s;
    return 0;
}

void csvnc_blit(csvnc_server *s, int x, int y, int w, int h,
                const void *px, int stride_bytes, int src_format)
{
    if (!s)
        return;
    csvnc_fb_blit(&s->fb, &x, &y, &w, &h, px, stride_bytes, src_format);
}

int csvnc_client_count(const csvnc_server *s)
{
    (void)s;
    return 0;
}

void csvnc_destroy(csvnc_server *s)
{
    if (!s)
        return;
    csvnc_fb_free(&s->fb);
    free(s);
}
