/* The lws protocol callback for protocol "cs-modbus".
 *
 * Connections carry a csmb_conn in the wsi opaque user data; the owning
 * engine is reachable via conn->owner (a csmb_slave / master, both
 * starting with a csmb_engine header).  Every activation that can emit
 * events flushes the engine's batch once at the end.
 *
 * TCP sockets use the RAW_* reasons; serial/fd connections use the
 * RAW_*_FILE reasons (for which lws does not hand over the data — the
 * fd must be read here). */

#include <libwebsockets.h>
#include <unistd.h>
#include <errno.h>
#include "csmb-private.h"

static int csmb_dispatch_rx(csmb_conn *conn, const uint8_t *buf, size_t len)
{
    switch (conn->role) {
    case CSMB_CONN_SLAVE_CHILD:
    case CSMB_CONN_SLAVE_SERIAL:
        return csmb_slave_on_rx((csmb_slave *)conn->owner, conn, buf, len);
    case CSMB_CONN_MASTER_TCP:
    case CSMB_CONN_MASTER_SERIAL:
        return csmb_master_on_rx((csmb_master *)conn->owner, buf, len);
    default:
        return 0;
    }
}

static void csmb_dispatch_close(csmb_conn *conn)
{
    switch (conn->role) {
    case CSMB_CONN_SLAVE_CHILD:
    case CSMB_CONN_SLAVE_SERIAL:
        csmb_slave_detach_conn((csmb_slave *)conn->owner, conn);
        break;
    case CSMB_CONN_MASTER_TCP:
    case CSMB_CONN_MASTER_SERIAL:
        csmb_master_on_closed((csmb_master *)conn->owner);
        break;
    default:
        break;
    }
}

int csmb_lws_protocol_callback(struct lws *wsi,
                               enum lws_callback_reasons reason,
                               void *user, void *in, size_t len)
{
    csmb_conn *conn;

    (void)user;

    switch (reason) {
    case LWS_CALLBACK_RAW_ADOPT: {
        /* A newly accepted socket on a slave listener vhost: the vhost
         * user pointer is the owning csmb_slave. */
        struct lws_vhost *vh = lws_get_vhost(wsi);
        void *owner = vh ? lws_vhost_user(vh) : NULL;

        if (!owner) {
            lwsl_warn("csmb: RAW_ADOPT on a vhost with no owner\n");
            return -1;
        }
        conn = csmb_slave_on_adopt((csmb_slave *)owner, wsi);
        if (!conn)
            return -1;
        csmb_event_flush((csmb_engine *)owner);
        return 0;
    }

    case LWS_CALLBACK_RAW_RX: {
        int ret;

        conn = lws_get_opaque_user_data(wsi);
        if (!conn || !conn->owner)
            return 0;   /* detached (owner destroyed): drop the bytes */
        ret = csmb_dispatch_rx(conn, in, len);
        csmb_event_flush((csmb_engine *)conn->owner);
        return ret;
    }

    case LWS_CALLBACK_RAW_RX_FILE: {
        /* serial/fd: lws does not pass the data; read it from the fd */
        uint8_t buf[1024];
        ssize_t n;
        int ret;

        conn = lws_get_opaque_user_data(wsi);
        if (!conn || !conn->owner)
            return 0;
        n = read(conn->fd, buf, sizeof(buf));
        if (n < 0)
            return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
        if (n == 0)
            return -1;   /* EOF: close */
        ret = csmb_dispatch_rx(conn, buf, (size_t)n);
        csmb_event_flush((csmb_engine *)conn->owner);
        return ret;
    }

    case LWS_CALLBACK_RAW_WRITEABLE:
    case LWS_CALLBACK_RAW_WRITEABLE_FILE:
        conn = lws_get_opaque_user_data(wsi);
        if (!conn)
            return 0;
        return csmb_conn_writable(conn, wsi);

    case LWS_CALLBACK_RAW_ADOPT_FILE:
        /* the engine set the connection up right after adopting the fd;
         * nothing to route here */
        return 0;

    case LWS_CALLBACK_RAW_CONNECTED:
        conn = lws_get_opaque_user_data(wsi);
        if (!conn || !conn->owner)
            return 0;
        if (conn->role == CSMB_CONN_MASTER_TCP) {
            csmb_master_on_connected((csmb_master *)conn->owner, wsi);
            csmb_event_flush((csmb_engine *)conn->owner);
        }
        return 0;

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        conn = lws_get_opaque_user_data(wsi);
        if (!conn)
            return 0;
        if (conn->owner && conn->role == CSMB_CONN_MASTER_TCP) {
            csmb_master_on_connect_error((csmb_master *)conn->owner);
            csmb_event_flush((csmb_engine *)conn->owner);
        }
        csmb_conn_free(conn);
        lws_set_opaque_user_data(wsi, NULL);
        return 0;

    case LWS_CALLBACK_RAW_CLOSE:
    case LWS_CALLBACK_RAW_CLOSE_FILE:
        conn = lws_get_opaque_user_data(wsi);
        if (!conn)
            return 0;
        if (conn->owner) {
            csmb_dispatch_close(conn);
            csmb_event_flush((csmb_engine *)conn->owner);
        }
        csmb_conn_free(conn);
        lws_set_opaque_user_data(wsi, NULL);
        return 0;

    default:
        return 0;
    }
}
