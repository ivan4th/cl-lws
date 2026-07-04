/* Shared transport glue: raw-skt client connect, listener vhost,
 * termios serial open, fd adoption, per-connection tx queue.
 * Filled in by the slave/master/RTU stages. */

#include <libwebsockets.h>
#include "csmb-private.h"

typedef int csmb_transport_placeholder;
