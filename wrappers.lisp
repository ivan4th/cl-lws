(in-package :lws)

(include "libwebsockets.h")

(defwrapper* set-mqtt-client-connect-param-flags
  :void
  ((param :pointer)
   (clean-start :int))
  "lws_mqtt_client_connect_param_t *p = param;
   p->clean_start = clean_start;
   p->client_id_nofree = 1;
   p->username_nofree = 1;
   p->password_nofree = 1;")

(defwrapper* set-mqtt-publish-param-flags
  :void
  ((param :pointer)
   (dup :int)
   (retain :int))
  "lws_mqtt_publish_param_t *p = param;
   p->dup = dup;
   p->retain = retain;")

(defwrapper* mqtt-publish-param-retain
  :int
  ((param :pointer))
  "lws_mqtt_publish_param_t *p = param;
   return p->retain;")

(defwrapper* mqtt-publish-param-dup
  :int
  ((param :pointer))
  "lws_mqtt_publish_param_t *p = param;
   return p->dup;")
