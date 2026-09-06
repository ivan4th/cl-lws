/* Internal structures shared across the csvnc sources.
 *
 * This header must stay free of libwebsockets.h so that the pure
 * translation units (rfb, pixfmt, encode, region, keys) and their unit
 * tests and fuzzers build without lws.  Only csvnc-lws.c talks to lws.
 */

#ifndef CSVNC_PRIVATE_H_INCLUDED
#define CSVNC_PRIVATE_H_INCLUDED

#include <stdlib.h>
#include <string.h>
#include "csvnc.h"

/* ---- RFB constants ---- */

#define CSVNC_ENC_RAW              0
#define CSVNC_ENC_HEXTILE          5
#define CSVNC_ENC_ZRLE             16
#define CSVNC_PSEUDO_DESKTOP_SIZE  (-223)
#define CSVNC_PSEUDO_LAST_RECT     (-224)

#define CSVNC_MSG_SET_PIXEL_FORMAT   0
#define CSVNC_MSG_SET_ENCODINGS      2
#define CSVNC_MSG_FB_UPDATE_REQUEST  3
#define CSVNC_MSG_KEY_EVENT          4
#define CSVNC_MSG_POINTER_EVENT      5
#define CSVNC_MSG_CLIENT_CUT_TEXT    6

#define CSVNC_SMSG_FB_UPDATE         0

#define CSVNC_HEXTILE_TILE   16
#define CSVNC_MAX_CUT_TEXT   (1u << 20)   /* larger ClientCutText = misbehaving client */
#define CSVNC_MAX_DIM        16384        /* sanity bound on framebuffer geometry */

/* ---- pixel formats (csvnc-pixfmt.c) ----
 *
 * A client pixel format as negotiated by SetPixelFormat, plus
 * translation tables compiled from it.  A shadow pixel 0x00RRGGBB
 * becomes rtab[R] | gtab[G] | btab[B]: the client pixel VALUE, which
 * csvnc_pix_put() then serialises in the client's byte order. */

typedef struct csvnc_pixfmt {
    uint8_t bpp, depth, big_endian, true_colour;
    uint16_t rmax, gmax, bmax;
    uint8_t rshift, gshift, bshift;
    /* derived */
    uint8_t bytespp;
    uint8_t host_order;   /* client byte order == host byte order */
    uint32_t rtab[256], gtab[256], btab[256];
} csvnc_pixfmt;

/* The server's native format: 32 bpp, depth 24, little-endian,
 * R shift 16, G 8, B 0 (XRGB8888). */
void csvnc_pixfmt_native(csvnc_pixfmt *f);
/* Parse the 16-byte wire PIXEL_FORMAT; 0 ok, -1 rejected (colour map,
 * odd bpp, channel does not fit, ...).  F is untouched on rejection. */
int csvnc_pixfmt_parse(csvnc_pixfmt *f, const uint8_t wire[16]);
/* Serialise as the 16-byte wire PIXEL_FORMAT. */
void csvnc_pixfmt_write(const csvnc_pixfmt *f, uint8_t wire[16]);

static inline uint32_t csvnc_pix_translate(const csvnc_pixfmt *f, uint32_t xrgb)
{
    return f->rtab[(xrgb >> 16) & 0xff] | f->gtab[(xrgb >> 8) & 0xff]
        | f->btab[xrgb & 0xff];
}

/* Store one client pixel value (bytespp bytes, client byte order). */
void csvnc_pix_put(const csvnc_pixfmt *f, uint8_t *dst, uint32_t v);
/* Translate + store N shadow pixels; returns dst + N * bytespp. */
uint8_t *csvnc_pix_put_row(const csvnc_pixfmt *f, uint8_t *dst,
                           const uint32_t *src, int n);
/* Read one client pixel value back (tests / reference decoders). */
uint32_t csvnc_pix_get(const csvnc_pixfmt *f, const uint8_t *src);

/* ---- shadow framebuffer ---- */

typedef struct csvnc_fb {
    uint32_t *px;        /* width * height, 0x00RRGGBB */
    int width, height;
} csvnc_fb;

int csvnc_fb_init(csvnc_fb *fb, int width, int height);  /* 0 / -1 */
void csvnc_fb_free(csvnc_fb *fb);
/* Blit as csvnc_blit(); returns the clipped rectangle in *X.. *H
 * (w or h 0 when nothing was copied). */
void csvnc_fb_blit(csvnc_fb *fb, int *x, int *y, int *w, int *h,
                   const void *px, int stride_bytes, int src_format);

/* ---- dirty regions (csvnc-region.c) ----
 *
 * A bounded list of rectangles covering everything that changed since
 * a client was last updated.  Adding a rectangle merges it with any
 * existing one when the union wastes no area (containment, adjacency,
 * heavy overlap); when the list is full the least wasteful merge is
 * forced; when the covered area exceeds most of the frame the whole
 * frame is used instead.  Rectangles never overlap the frame bounds. */

#define CSVNC_REGION_MAX 16

typedef struct csvnc_rect {
    int x, y, w, h;
} csvnc_rect;

typedef struct csvnc_region {
    csvnc_rect r[CSVNC_REGION_MAX];
    int n;
    int fw, fh;          /* frame size (clip bounds) */
} csvnc_region;

void csvnc_region_init(csvnc_region *rg, int fw, int fh);
void csvnc_region_clear(csvnc_region *rg);
void csvnc_region_add(csvnc_region *rg, int x, int y, int w, int h);
void csvnc_region_add_all(csvnc_region *rg);
int csvnc_region_empty(const csvnc_region *rg);
long csvnc_region_area(const csvnc_region *rg);
/* Remove and return one rectangle, at most MAX_ROWS tall (a taller one
 * is split and its remainder kept).  Returns 0 when empty. */
int csvnc_region_pop(csvnc_region *rg, int max_rows, csvnc_rect *out);

/* ---- encoders (csvnc-encode.c) ----
 *
 * Encoders write a complete FramebufferUpdate rectangle (12-byte
 * header + payload) into a caller-provided buffer and return its size,
 * or 0 when it does not fit CAP; csvnc_encode_max_size() is the bound
 * to size the buffer with.  Raw and Hextile are stateless; ZRLE (M3)
 * will keep its zlib stream in csvnc_encoder. */

typedef struct csvnc_encoder {
    int32_t enc;               /* CSVNC_ENC_* in use */
    void *zrle;                /* reserved for the ZRLE stream */
} csvnc_encoder;

void csvnc_encoder_init(csvnc_encoder *e);
void csvnc_encoder_free(csvnc_encoder *e);
/* Select an encoding; unsupported values fall back to Raw. */
void csvnc_encoder_select(csvnc_encoder *e, int32_t enc);

size_t csvnc_encode_max_size(const csvnc_encoder *e, const csvnc_pixfmt *f,
                             int w, int h);
size_t csvnc_encode_rect(csvnc_encoder *e, const csvnc_pixfmt *f,
                         const csvnc_fb *fb, int x, int y, int w, int h,
                         uint8_t *out, size_t cap);

/* Payload-only encoders (no rectangle header); 0 = does not fit. */
size_t csvnc_encode_raw(const csvnc_pixfmt *f, const csvnc_fb *fb,
                        int x, int y, int w, int h,
                        uint8_t *out, size_t cap);
size_t csvnc_encode_hextile(const csvnc_pixfmt *f, const csvnc_fb *fb,
                            int x, int y, int w, int h,
                            uint8_t *out, size_t cap);
size_t csvnc_hextile_max_size(const csvnc_pixfmt *f, int w, int h);

/* Server-to-client message pieces. */
#define CSVNC_UPDATE_HDR_SIZE 4
#define CSVNC_RECT_HDR_SIZE   12
void csvnc_put_update_header(uint8_t *out, unsigned nrects);
void csvnc_put_rect_header(uint8_t *out, int x, int y, int w, int h,
                           int32_t enc);

/* ---- RFB protocol state machine (csvnc-rfb.c) ----
 *
 * One csvnc_rfb per client connection.  csvnc_rfb_start() emits the
 * server's ProtocolVersion; csvnc_rfb_feed() consumes any amount of
 * client bytes, driving the handshake (version, security type None,
 * ClientInit -> ServerInit) and then parsing client messages into the
 * ops callbacks.  Handshake output goes through ops->send.  Input may
 * be fragmented arbitrarily; variable-length payloads (SetEncodings,
 * ClientCutText) are streamed, so the parser never buffers more than
 * a fixed message head.  Any protocol violation returns -1 and the
 * connection must be dropped. */

typedef struct csvnc_rfb_ops {
    void (*send)(void *user, const uint8_t *data, size_t len);
    /* SetPixelFormat accepted; rfb->fmt already updated. */
    void (*pixel_format)(void *user);
    /* SetEncodings fully parsed; rfb->enc / rfb->lastrect updated. */
    void (*encodings)(void *user);
    void (*update_request)(void *user, int incremental,
                           int x, int y, int w, int h);
    void (*key)(void *user, uint32_t keysym, int down);
    void (*pointer)(void *user, int x, int y, unsigned buttons);
} csvnc_rfb_ops;

enum csvnc_rfb_state {
    CSVNC_ST_VERSION = 0,
    CSVNC_ST_SECURITY_TYPE,
    CSVNC_ST_CLIENT_INIT,
    CSVNC_ST_MSG_TYPE,
    CSVNC_ST_MSG_BODY,
    CSVNC_ST_ENCODINGS,
    CSVNC_ST_CUT_TEXT,
    CSVNC_ST_FAILED
};

typedef struct csvnc_rfb {
    const csvnc_rfb_ops *ops;
    void *user;
    int width, height;
    const char *name;
    int state;
    int version;            /* 33, 37 or 38 once negotiated */
    uint8_t buf[20];        /* head of the message being assembled */
    unsigned have, need;
    uint8_t msg;            /* client message type being parsed */
    uint32_t remaining;     /* streamed payload bytes still expected */
    /* negotiated client state */
    csvnc_pixfmt fmt;       /* current client pixel format */
    int32_t enc;            /* chosen framebuffer encoding */
    int lastrect;           /* client accepts the LastRect pseudo-encoding */
    int enc_chosen;         /* an encoding was picked in this SetEncodings */
} csvnc_rfb;

void csvnc_rfb_init(csvnc_rfb *c, const csvnc_rfb_ops *ops, void *user,
                    int width, int height, const char *name);
void csvnc_rfb_start(csvnc_rfb *c);
int csvnc_rfb_feed(csvnc_rfb *c, const uint8_t *data, size_t len);
static inline int csvnc_rfb_established(const csvnc_rfb *c)
{
    return c->state >= CSVNC_ST_MSG_TYPE && c->state != CSVNC_ST_FAILED;
}

/* ---- byte helpers ---- */

static inline void csvnc_put_u16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void csvnc_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static inline unsigned csvnc_get_u16(const uint8_t *p)
{
    return ((unsigned)p[0] << 8) | p[1];
}

static inline uint32_t csvnc_get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
        | ((uint32_t)p[2] << 8) | p[3];
}

#endif /* CSVNC_PRIVATE_H_INCLUDED */
