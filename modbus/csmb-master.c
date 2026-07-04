/* Master engine: transport FSM, reconnect, request pump, response
 * dispatch, staleness timers, event emission.
 * Filled in by the master stage. */

#include <libwebsockets.h>
#include "csmb-private.h"

typedef int csmb_master_placeholder;
