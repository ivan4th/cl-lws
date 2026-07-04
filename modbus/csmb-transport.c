/* Shared transport glue: per-connection tx queue (one lws_write per
 * WRITEABLE, partial-write handling) and the slave listener vhost.
 * Serial / fd transports and the master client connect land here in a
 * later stage. */

#include <libwebsockets.h>
#include "csmb-private.h"

/* ---- per-connection tx queue ---- */

int csmb_conn_send(csmb_conn *conn, const uint8_t *frame, size_t len)
{
    csmb_txbuf *b = malloc(sizeof(*b) + LWS_PRE + len);

    if (!b)
        return CSMB_ENOMEM;
    b->next = NULL;
    b->len = len;
    b->off = 0;
    memcpy(b->data + LWS_PRE, frame, len);
    if (conn->tx_tail)
        conn->tx_tail->next = b;
    else
        conn->tx_head = b;
    conn->tx_tail = b;
    if (conn->wsi)
        lws_callback_on_writable(conn->wsi);
    return CSMB_OK;
}

int csmb_conn_writable(csmb_conn *conn, struct lws *wsi)
{
    csmb_txbuf *b = conn->tx_head;
    uint8_t *payload;
    size_t remaining;
    int written;

    if (!b)
        return conn->close_requested ? -1 : 0;

    payload = b->data + LWS_PRE + b->off;
    remaining = b->len - b->off;
    written = lws_write(wsi, payload, remaining, LWS_WRITE_RAW);
    if (written < 0)
        return -1;
    b->off += (size_t)written;
    if (b->off >= b->len) {
        conn->tx_head = b->next;
        if (!conn->tx_head)
            conn->tx_tail = NULL;
        free(b);
    }
    if (conn->tx_head)
        lws_callback_on_writable(wsi);
    else if (conn->close_requested)
        return -1;
    return 0;
}

void csmb_conn_free(csmb_conn *conn)
{
    csmb_txbuf *b = conn->tx_head;

    while (b) {
        csmb_txbuf *bn = b->next;
        free(b);
        b = bn;
    }
    free(conn);
}

/* ---- slave listener vhost ----
 *
 * One explicit vhost per listener (cl-lws raw-listen style).  lws keeps
 * the vhost-name / listen_accept_* / iface string pointers without
 * copying, so they are heap-allocated with the listener's lifetime.  The
 * owning engine goes in the vhost user pointer so LWS_CALLBACK_RAW_ADOPT
 * can route the accepted socket. */

int csmb_transport_listen(csmb_listener *ln, struct lws_context *cx,
                          const struct lws_protocols *protocols,
                          const csmb_transport *tr, void *owner)
{
    static unsigned seq;
    struct lws_context_creation_info info;
    char namebuf[48];

    memset(ln, 0, sizeof(*ln));
    snprintf(namebuf, sizeof(namebuf), "csmb-slave-%u", ++seq);
    ln->vhost_name = strdup(namebuf);
    ln->role_str = strdup("raw-skt");
    ln->proto_str = strdup("cs-modbus");
    if (tr->host_or_device)
        ln->iface_str = strdup(tr->host_or_device);
    if (!ln->vhost_name || !ln->role_str || !ln->proto_str ||
        (tr->host_or_device && !ln->iface_str)) {
        csmb_transport_listen_close(ln);
        return CSMB_ENOMEM;
    }

    memset(&info, 0, sizeof(info));
    info.port = tr->port;
    info.iface = ln->iface_str;            /* NULL = any interface */
    info.protocols = protocols;
    info.options = LWS_SERVER_OPTION_ADOPT_APPLY_LISTEN_ACCEPT_CONFIG;
    info.listen_accept_role = ln->role_str;
    info.listen_accept_protocol = ln->proto_str;
    info.vhost_name = ln->vhost_name;
    info.user = owner;

    ln->vhost = lws_create_vhost(cx, &info);
    if (!ln->vhost) {
        csmb_transport_listen_close(ln);
        return CSMB_ETRANSPORT;
    }
    return CSMB_OK;
}

void csmb_transport_listen_close(csmb_listener *ln)
{
    if (ln->vhost) {
        lws_vhost_destroy(ln->vhost);
        ln->vhost = NULL;
    }
    free(ln->vhost_name);
    free(ln->role_str);
    free(ln->proto_str);
    free(ln->iface_str);
    ln->vhost_name = ln->role_str = ln->proto_str = ln->iface_str = NULL;
}

/* ---- master outgoing TCP connect ----
 *
 * A raw-skt client connection bound to the "cs-modbus" protocol on
 * CLIENT_VHOST; the csmb_conn is the wsi's opaque user data, so
 * RAW_CONNECTED / CLIENT_CONNECTION_ERROR / RAW_RX / RAW_CLOSE route
 * back to it. */

csmb_conn *csmb_transport_connect(struct lws_context *cx,
                                  struct lws_vhost *client_vhost,
                                  const csmb_transport *tr, void *owner)
{
    struct lws_client_connect_info info;
    csmb_conn *conn = calloc(1, sizeof(*conn));
    struct lws *wsi;

    if (!conn)
        return NULL;
    conn->role = CSMB_CONN_MASTER_TCP;
    conn->owner = owner;
    csmb_mbap_parser_init(&conn->mbap);

    memset(&info, 0, sizeof(info));
    info.context = cx;
    info.vhost = client_vhost;
    info.address = tr->host_or_device;
    info.host = tr->host_or_device;
    info.port = tr->port;
    info.method = "RAW";
    info.local_protocol_name = "cs-modbus";
    info.opaque_user_data = conn;
    info.pwsi = &wsi;

    wsi = lws_client_connect_via_info(&info);
    if (!wsi) {
        free(conn);
        return NULL;
    }
    conn->wsi = wsi;
    lws_set_opaque_user_data(wsi, conn);
    return conn;
}
