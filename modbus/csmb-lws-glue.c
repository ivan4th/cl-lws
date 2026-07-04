/* Accessors for libwebsockets MQTT param struct bitfields, which FFI
 * cannot address directly.  Formerly a cffi wrapper library
 * (wrappers.lisp); living in libcsmodbus keeps the Lisp side to two
 * soname-loaded shared libraries, so saved images can reopen them via
 * the plain dynamic-linker search path (ldconfig, packelf
 * --library-path) instead of absolute build-time paths. */

#include <libwebsockets.h>

void csmb_glue_set_mqtt_client_connect_param_flags(void *param, int clean_start)
{
    lws_mqtt_client_connect_param_t *p = param;

    p->clean_start = clean_start;
    p->client_id_nofree = 1;
    p->username_nofree = 1;
    p->password_nofree = 1;
}

void csmb_glue_set_mqtt_publish_param_flags(void *param, int dup, int retain)
{
    lws_mqtt_publish_param_t *p = param;

    p->dup = dup;
    p->retain = retain;
}

int csmb_glue_mqtt_publish_param_retain(void *param)
{
    lws_mqtt_publish_param_t *p = param;

    return p->retain;
}

int csmb_glue_mqtt_publish_param_dup(void *param)
{
    lws_mqtt_publish_param_t *p = param;

    return p->dup;
}
