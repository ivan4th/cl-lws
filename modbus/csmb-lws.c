/* The lws protocol callback for protocol "cs-modbus".
 *
 * All cs-modbus connections (master TCP client, slave TCP listener
 * children, serial/fd adoptions) carry a csmb_conn pointer in the wsi
 * opaque user data; dispatch fans out to the master / slave engines.
 *
 * Skeleton stage: connection routing is filled in by the master/slave
 * engine work; for now unknown connections are dropped.
 */

#include <libwebsockets.h>
#include "csmb-private.h"

int csmb_lws_protocol_callback(struct lws *wsi,
                               enum lws_callback_reasons reason,
                               void *user, void *in, size_t len)
{
    (void)user;
    (void)in;
    (void)len;

    switch (reason) {
    case LWS_CALLBACK_RAW_ADOPT:
    case LWS_CALLBACK_RAW_CONNECTED:
    case LWS_CALLBACK_RAW_RX:
    case LWS_CALLBACK_RAW_WRITEABLE:
    case LWS_CALLBACK_RAW_CLOSE:
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
    case LWS_CALLBACK_RAW_ADOPT_FILE:
    case LWS_CALLBACK_RAW_RX_FILE:
    case LWS_CALLBACK_RAW_WRITEABLE_FILE:
    case LWS_CALLBACK_RAW_CLOSE_FILE:
        if (!lws_get_opaque_user_data(wsi)) {
            lwsl_warn("csmb: callback %d on unbound wsi\n", (int)reason);
            return -1;
        }
        /* engine dispatch lands here in the master/slave stages */
        return 0;
    default:
        return 0;
    }
}
