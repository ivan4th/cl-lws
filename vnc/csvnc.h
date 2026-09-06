/*
 * csvnc — a small RFB (VNC) server for cl-lws.
 *
 * Serves a shadow framebuffer over RFB 3.3 / 3.7 / 3.8 (security type
 * None only, always shared) from inside a libwebsockets service loop,
 * so it can run threadless next to the LVGL console pump.  The host
 * feeds pixels with csvnc_blit() (from the display flush path) and
 * receives keyboard/pointer input through callbacks.  Encodings: Raw,
 * Hextile (ZRLE planned).  No cursor, DesktopSize or clipboard support.
 *
 * Threading: everything runs on the single lws service thread.  No API
 * is thread-safe; marshal calls through the event loop.
 *
 * Callbacks are invoked from inside lws_service(); they must not block
 * and must not call csvnc_destroy().
 */

#ifndef CSVNC_H_INCLUDED
#define CSVNC_H_INCLUDED

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct lws;
struct lws_context;
struct lws_protocols;

/* Source pixel formats accepted by csvnc_blit().  The shadow itself is
 * always XRGB8888: one uint32_t per pixel holding 0x00RRGGBB in host
 * byte order (bytes B,G,R,X in memory on a little-endian machine). */
enum {
    CSVNC_SRC_XRGB8888 = 0,   /* uint32_t per pixel, as the shadow */
    CSVNC_SRC_RGB565   = 1    /* uint16_t per pixel, R5 G6 B5 */
};

typedef struct csvnc_callbacks {
    /* A KeyEvent from a client, already translated to an SDL2 keycode
     * (SDLK_*: printable ASCII as the character, others as
     * 0x40000000 | scancode, see csvnc_keysym_to_sdl()).  Keysyms with
     * no mapping are dropped before this is called.  Optional. */
    void (*key)(void *user, int32_t sdl_key, int32_t down);
    /* A PointerEvent; buttons is the RFB button mask (bit 0 = left).
     * Optional (NULL = ignore pointer input). */
    void (*pointer)(void *user, int x, int y, unsigned buttons);
} csvnc_callbacks;

typedef struct csvnc_server csvnc_server;

/* Create a server listening on IFACE:PORT (iface NULL = all
 * interfaces, port 0 = ephemeral, see csvnc_listen_port()) exporting
 * a WIDTH x HEIGHT framebuffer, initially black.  PROTOCOLS is the
 * context's protocol array, which must contain "cs-vnc" bound to
 * csvnc_lws_protocol_callback.  CBS is copied (NULL = no callbacks).
 * Returns NULL on failure (bad geometry, port in use, out of
 * memory). */
csvnc_server *csvnc_create(struct lws_context *cx,
                           const struct lws_protocols *protocols,
                           const char *iface, int port,
                           int width, int height,
                           const csvnc_callbacks *cbs, void *user);

/* The lws protocol callback to register under the name "cs-vnc". */
int csvnc_lws_protocol_callback(struct lws *wsi, int reason,
                                void *user, void *in, size_t len);

/* The port actually bound (useful with port 0). */
int csvnc_listen_port(const csvnc_server *s);

/* Copy a W x H block of pixels at (X, Y) into the shadow framebuffer
 * and mark what actually changed dirty for every client (the block is
 * compared against the shadow, so a display that flushes the whole
 * screen for every change still yields small updates).  PX points at
 * the top-left source pixel, rows are STRIDE_BYTES apart, SRC_FORMAT
 * is one of CSVNC_SRC_*.  The rectangle is clipped to the framebuffer.
 * Cheap enough to call from a display flush callback: one compare +
 * copy (or 565->8888 expansion) pass over the area. */
void csvnc_blit(csvnc_server *s, int x, int y, int w, int h,
                const void *px, int stride_bytes, int src_format);

/* Number of clients that completed the handshake. */
int csvnc_client_count(const csvnc_server *s);

/* Drop all clients, close the listener, free everything. */
void csvnc_destroy(csvnc_server *s);

/* X keysym -> SDL2 keycode (SDLK_*), 0 when unmapped.  Latin-1
 * printable keysyms map to themselves (upper-case letters to the
 * lower-case SDL keycode); keypad digits and operators map to the
 * ASCII characters, matching the console's evdev key path. */
int32_t csvnc_keysym_to_sdl(uint32_t keysym);

#ifdef __cplusplus
}
#endif

#endif /* CSVNC_H_INCLUDED */
