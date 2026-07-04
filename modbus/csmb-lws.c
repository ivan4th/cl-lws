/* The lws protocol callback for protocol "cs-modbus".
 *
 * Accepted slave sockets carry a csmb_conn in the wsi opaque user data;
 * the owning engine is reachable via conn->owner (a csmb_slave / master,
 * both starting with a csmb_engine header).  Every activation that can
 * emit events flushes the engine's batch once at the end.
 *
 * The master TCP client (RAW_CONNECTED / CLIENT_CONNECTION_ERROR) is a
 * later stage; those reasons are accepted but ignored here. */

#include <libwebsockets.h>
#include "csmb-private.h"

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
        int ret = 0;

        conn = lws_get_opaque_user_data(wsi);
        if (!conn || !conn->owner)
            return 0;   /* detached (owner destroyed): drop the bytes */
        switch (conn->role) {
        case CSMB_CONN_SLAVE_CHILD:
            ret = csmb_slave_on_rx((csmb_slave *)conn->owner, conn, in, len);
            break;
        default:
            break;
        }
        csmb_event_flush((csmb_engine *)conn->owner);
        return ret;
    }

    case LWS_CALLBACK_RAW_WRITEABLE:
        conn = lws_get_opaque_user_data(wsi);
        if (!conn)
            return 0;
        return csmb_conn_writable(conn, wsi);

    case LWS_CALLBACK_RAW_CLOSE:
        conn = lws_get_opaque_user_data(wsi);
        if (!conn)
            return 0;
        if (conn->owner) {
            switch (conn->role) {
            case CSMB_CONN_SLAVE_CHILD:
                csmb_slave_detach_conn((csmb_slave *)conn->owner, conn);
                break;
            default:
                break;
            }
        }
        csmb_conn_free(conn);
        lws_set_opaque_user_data(wsi, NULL);
        return 0;

    case LWS_CALLBACK_RAW_CONNECTED:
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        /* master TCP client: later stage */
        return 0;

    default:
        return 0;
    }
}
