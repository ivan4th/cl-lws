/* Raw and Hextile encoders: round trip through a reference Hextile
 * decoder written against the RFB spec, over random, flat and mostly
 * flat frames in several client formats and odd sizes; size bounds;
 * the byte costs the console relies on; rectangle/update headers. */

#include "../csvnc-private.h"
#include "test-util.h"
#include "zrle-ref.h"

static uint32_t rng_state = 4242;

static uint32_t rng(void)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return rng_state >> 8;
}

/* ---- reference Hextile decoder ----
 *
 * Decodes payload bytes for a W x H rectangle into OUT (client pixel
 * values, row-major).  Returns the bytes consumed or -1 on a malformed
 * stream.  Also enforces what noVNC needs: subencoding <= 30 and never
 * a blank (0) tile straight after a raw one. */

static long ref_hextile_decode(const csvnc_pixfmt *f, const uint8_t *in,
                               size_t len, int w, int h, uint32_t *out)
{
    size_t pos = 0;
    uint32_t bg = 0, fg = 0;
    int have_bg = 0, have_fg = 0, last_raw = 0;
    int ty, tx;
    unsigned bpp = f->bytespp;

    for (ty = 0; ty < h; ty += 16) {
        int th = h - ty < 16 ? h - ty : 16;

        for (tx = 0; tx < w; tx += 16) {
            int tw = w - tx < 16 ? w - tx : 16;
            unsigned sub;
            int x, y;

            if (pos >= len)
                return -1;
            sub = in[pos++];
            if (sub > 30)
                return -1;
            if (sub & 1) {
                if (pos + (size_t)tw * th * bpp > len)
                    return -1;
                for (y = 0; y < th; y++)
                    for (x = 0; x < tw; x++) {
                        out[(ty + y) * w + tx + x] = csvnc_pix_get(f, in + pos);
                        pos += bpp;
                    }
                last_raw = 1;
                continue;
            }
            if (sub == 0 && last_raw)
                return -1;           /* noVNC ignores this tile */
            last_raw = 0;
            if (sub & 2) {
                if (pos + bpp > len)
                    return -1;
                bg = csvnc_pix_get(f, in + pos);
                pos += bpp;
                have_bg = 1;
            }
            if (sub & 4) {
                if (pos + bpp > len)
                    return -1;
                fg = csvnc_pix_get(f, in + pos);
                pos += bpp;
                have_fg = 1;
            }
            if (!have_bg)
                return -1;
            for (y = 0; y < th; y++)
                for (x = 0; x < tw; x++)
                    out[(ty + y) * w + tx + x] = bg;
            if (sub & 8) {
                unsigned n, i;

                if (pos >= len)
                    return -1;
                n = in[pos++];
                for (i = 0; i < n; i++) {
                    uint32_t c = fg;
                    unsigned sx, sy, sw, sh;

                    if (sub & 16) {
                        if (pos + bpp > len)
                            return -1;
                        c = csvnc_pix_get(f, in + pos);
                        pos += bpp;
                    } else if (!have_fg) {
                        return -1;
                    }
                    if (pos + 2 > len)
                        return -1;
                    sx = in[pos] >> 4;
                    sy = in[pos] & 15;
                    sw = (in[pos + 1] >> 4) + 1;
                    sh = (in[pos + 1] & 15) + 1;
                    pos += 2;
                    if (sx + sw > (unsigned)tw || sy + sh > (unsigned)th)
                        return -1;
                    for (y = 0; y < (int)sh; y++)
                        for (x = 0; x < (int)sw; x++)
                            out[(ty + (int)sy + y) * w + tx + (int)sx + x] = c;
                }
            }
        }
    }
    return (long)pos;
}

/* ---- frames ---- */

typedef enum { FR_NOISE, FR_FLAT, FR_DOTS, FR_TEXT, FR_GRADIENT, FR_BLOCKS, FR_N } frame_kind;

static void fill_frame(csvnc_fb *fb, frame_kind kind)
{
    int x, y;
    uint32_t base = rng() & 0xffffff;

    for (y = 0; y < fb->height; y++) {
        for (x = 0; x < fb->width; x++) {
            uint32_t v;

            switch (kind) {
            case FR_NOISE:
                v = rng() & 0xffffff;
                break;
            case FR_FLAT:
                v = base;
                break;
            case FR_DOTS:
                v = (rng() % 97 == 0) ? (rng() & 0xffffff) : base;
                break;
            case FR_TEXT:   /* two colours, blobby */
                v = ((x / 3 + y / 5) % 3 == 0 || rng() % 11 == 0) ? 0xffffff : base;
                break;
            case FR_GRADIENT:
                v = ((uint32_t)(x & 0xff) << 16) | ((uint32_t)(y & 0xff) << 8)
                    | ((uint32_t)(x + y) & 0xff);
                break;
            default:        /* 8x8 blocks of a few colours */
                v = 0x111111u * (uint32_t)(((x / 8) * 3 + (y / 8) * 7) % 6);
                break;
            }
            fb->px[y * fb->width + x] = v;
        }
    }
}

static const struct {
    int bpp, depth, be, rmax, gmax, bmax, rs, gs, bs;
} formats[] = {
    { 32, 24, 0, 255, 255, 255, 16, 8, 0 },    /* native */
    { 32, 24, 0, 255, 255, 255, 0, 8, 16 },    /* noVNC */
    { 32, 24, 1, 255, 255, 255, 16, 8, 0 },    /* big-endian */
    { 16, 16, 0, 31, 63, 31, 11, 5, 0 },       /* RGB565 LE */
    { 16, 16, 1, 31, 63, 31, 11, 5, 0 },       /* RGB565 BE */
    { 8, 8, 0, 7, 7, 3, 5, 2, 0 },             /* RGB332 */
};
#define NFORMATS (sizeof(formats) / sizeof(formats[0]))

static void make_fmt(csvnc_pixfmt *f, size_t i)
{
    uint8_t w[16];

    memset(w, 0, 16);
    w[0] = (uint8_t)formats[i].bpp;
    w[1] = (uint8_t)formats[i].depth;
    w[2] = (uint8_t)formats[i].be;
    w[3] = 1;
    csvnc_put_u16(w + 4, (unsigned)formats[i].rmax);
    csvnc_put_u16(w + 6, (unsigned)formats[i].gmax);
    csvnc_put_u16(w + 8, (unsigned)formats[i].bmax);
    w[10] = (uint8_t)formats[i].rs;
    w[11] = (uint8_t)formats[i].gs;
    w[12] = (uint8_t)formats[i].bs;
    TCHECK_EQ(csvnc_pixfmt_parse(f, w), 0);
}

/* Encode (x,y,w,h) of FB with Hextile, decode, compare against the
 * translated shadow.  Returns the encoded size. */
static size_t hextile_round_trip(const csvnc_pixfmt *f, const csvnc_fb *fb,
                                 int x, int y, int w, int h)
{
    size_t cap = csvnc_hextile_max_size(f, w, h);
    uint8_t *buf = malloc(cap + 16);
    uint32_t *dec = malloc((size_t)w * h * sizeof(uint32_t));
    size_t n;
    long used;
    int i, j, bad = 0;

    memset(buf, 0xa5, cap + 16);
    n = csvnc_encode_hextile(f, fb, x, y, w, h, buf, cap);
    TCHECK(n > 0);
    TCHECK(n <= cap);
    for (i = 0; i < 16; i++)
        TCHECK_EQ(buf[cap + i], 0xa5);
    used = ref_hextile_decode(f, buf, n, w, h, dec);
    TCHECK_EQ(used, (long)n);
    if (used == (long)n) {
        for (j = 0; j < h && !bad; j++)
            for (i = 0; i < w; i++) {
                uint32_t expect = csvnc_pix_translate(f, fb->px[(y + j) * fb->width + x + i]);

                if (dec[j * w + i] != expect) {
                    TFAIL("pixel (%d,%d) of %dx%d rect at (%d,%d): %x != %x",
                          i, j, w, h, x, y, dec[j * w + i], expect);
                    bad = 1;
                    break;
                }
            }
    }
    free(buf);
    free(dec);
    return n;
}

static void test_hextile_round_trip(void)
{
    static const struct { int w, h; } sizes[] = {
        { 16, 16 }, { 32, 32 }, { 1, 1 }, { 17, 16 }, { 16, 17 }, { 37, 23 },
        { 100, 3 }, { 3, 100 }, { 15, 15 }, { 64, 48 }, { 129, 65 },
    };
    size_t fi, si;
    int kind;

    for (fi = 0; fi < NFORMATS; fi++) {
        csvnc_pixfmt f;

        make_fmt(&f, fi);
        for (kind = 0; kind < FR_N; kind++) {
            for (si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
                csvnc_fb fb;
                int w = sizes[si].w, h = sizes[si].h;

                TCHECK_EQ(csvnc_fb_init(&fb, w + 7, h + 5), 0);
                fill_frame(&fb, (frame_kind)kind);
                /* whole frame */
                hextile_round_trip(&f, &fb, 0, 0, fb.width, fb.height);
                /* an interior rectangle at an odd offset */
                hextile_round_trip(&f, &fb, 3, 2, w, h);
                /* a rectangle touching the bottom-right corner */
                hextile_round_trip(&f, &fb, 7, 5, w, h);
                csvnc_fb_free(&fb);
            }
        }
    }
}

static void test_hextile_full_frame(void)
{
    /* the console frame size, random and mostly flat, noVNC format */
    csvnc_pixfmt f;
    csvnc_fb fb;
    size_t n;

    make_fmt(&f, 1);
    TCHECK_EQ(csvnc_fb_init(&fb, 640, 480), 0);
    fill_frame(&fb, FR_NOISE);
    n = hextile_round_trip(&f, &fb, 0, 0, 640, 480);
    /* noise is all raw tiles: raw size + one byte per tile */
    TCHECK_EQ(n, 640 * 480 * 4 + 40 * 30);
    fill_frame(&fb, FR_DOTS);
    n = hextile_round_trip(&f, &fb, 0, 0, 640, 480);
    TCHECK(n < 640 * 480 * 4 / 20);
    fill_frame(&fb, FR_FLAT);
    n = hextile_round_trip(&f, &fb, 0, 0, 640, 480);
    /* first tile 1 + 4, every other tile 1 byte */
    TCHECK_EQ(n, 5 + 40 * 30 - 1);
    csvnc_fb_free(&fb);
}

static void test_hextile_costs(void)
{
    csvnc_pixfmt f;
    csvnc_fb fb;
    uint8_t buf[4096];
    size_t n;
    int i;

    csvnc_pixfmt_native(&f);
    TCHECK_EQ(csvnc_fb_init(&fb, 48, 16), 0);
    for (i = 0; i < 48 * 16; i++)
        fb.px[i] = 0x00336699;
    /* flat: bg specified once, then 1 byte per tile */
    n = csvnc_encode_hextile(&f, &fb, 0, 0, 32, 16, buf, sizeof(buf));
    TCHECK_EQ(n, 6);
    TCHECK_EQ(buf[0], 2);             /* BackgroundSpecified */
    TCHECK_EQ(buf[5], 0);
    /* one different pixel in the second tile: bg carried, fg + 1 subrect */
    fb.px[3 * 48 + 20] = 0x00ffffff;
    n = csvnc_encode_hextile(&f, &fb, 0, 0, 32, 16, buf, sizeof(buf));
    TCHECK_EQ(n, 5 + 1 + 4 + 1 + 2);
    TCHECK_EQ(buf[5], 4 | 8);         /* ForegroundSpecified | AnySubrects */
    TCHECK_EQ(buf[10], 1);
    TCHECK_EQ(buf[11], (4 << 4) | 3); /* x=4,y=3 within the tile */
    TCHECK_EQ(buf[12], 0);            /* 1x1 */
    /* a 2-pixel-wide, 3-tall bar of the same colour: still 1 subrect */
    fb.px[3 * 48 + 21] = 0x00ffffff;
    fb.px[4 * 48 + 20] = 0x00ffffff;
    fb.px[4 * 48 + 21] = 0x00ffffff;
    fb.px[5 * 48 + 20] = 0x00ffffff;
    fb.px[5 * 48 + 21] = 0x00ffffff;
    n = csvnc_encode_hextile(&f, &fb, 0, 0, 32, 16, buf, sizeof(buf));
    TCHECK_EQ(n, 13);
    TCHECK_EQ(buf[12], (1 << 4) | 2); /* 2x3 */
    /* third tile: same two colours -> nothing to specify, 1 + 1 + 2 */
    fb.px[2 * 48 + 40] = 0x00ffffff;
    n = csvnc_encode_hextile(&f, &fb, 0, 0, 48, 16, buf, sizeof(buf));
    TCHECK_EQ(n, 13 + 4);
    TCHECK_EQ(buf[13], 8);
    /* a third colour: coloured subrects */
    fb.px[2 * 48 + 41] = 0x00ff0000;
    n = csvnc_encode_hextile(&f, &fb, 0, 0, 48, 16, buf, sizeof(buf));
    TCHECK_EQ(n, 13 + 1 + 1 + 2 * (4 + 2));
    TCHECK_EQ(buf[13], 8 | 16);
    /* a tile whose bg changes but fg stays: only the bg restated */
    for (i = 0; i < 16; i++)
        fb.px[i * 48 + 32 + (i % 16)] = 0x00ffffff;   /* keep white present */
    for (i = 0; i < 16 * 16; i++)
        if (fb.px[(i / 16) * 48 + 32 + i % 16] != 0x00ffffff)
            fb.px[(i / 16) * 48 + 32 + i % 16] = 0x00000000;
    n = csvnc_encode_hextile(&f, &fb, 0, 0, 48, 16, buf, sizeof(buf));
    TCHECK_EQ(buf[13], 2 | 8);
    /* 16 diagonal pixels plus the one left over at (40,2): 17 subrects */
    TCHECK_EQ(n, 13 + 1 + 4 + 1 + 17 * 2);
    /* a two-colour checkerboard is still cheaper than raw at 32 bpp:
     * 128 one-pixel subrects at 2 bytes each */
    for (i = 0; i < 48 * 16; i++)
        fb.px[i] = ((i % 48) + (i / 48)) & 1 ? 0x00ffffff : 0;
    n = csvnc_encode_hextile(&f, &fb, 0, 0, 16, 16, buf, sizeof(buf));
    TCHECK_EQ(n, 1 + 4 + 4 + 1 + 128 * 2);
    TCHECK_EQ(buf[0], 2 | 4 | 8);
    /* raw fallback when coloured subrects would cost more: four colours
     * with no runs in either direction (192 subrects at 6 bytes) */
    for (i = 0; i < 48 * 16; i++)
        fb.px[i] = 0x00202020u * (uint32_t)(((i % 48) + 2 * (i / 48)) % 4 + 1);
    n = csvnc_encode_hextile(&f, &fb, 0, 0, 16, 16, buf, sizeof(buf));
    TCHECK_EQ(n, 1 + 256 * 4);
    TCHECK_EQ(buf[0], 1);
    /* and after a raw tile a flat tile restates its background
     * (never a bare 0, which noVNC would ignore) */
    for (i = 0; i < 16; i++)
        memset(fb.px + i * 48 + 16, 0, 16 * 4);
    n = csvnc_encode_hextile(&f, &fb, 0, 0, 32, 16, buf, sizeof(buf));
    TCHECK_EQ(n, 1 + 256 * 4 + 5);
    TCHECK_EQ(buf[1 + 256 * 4], 2);
    /* buffer too small: 0, nothing written past cap */
    memset(buf, 0xee, sizeof(buf));
    n = csvnc_encode_hextile(&f, &fb, 0, 0, 32, 16, buf, 100);
    TCHECK_EQ(n, 0);
    for (i = 100; i < 200; i++)
        TCHECK_EQ(buf[i], 0xee);
    csvnc_fb_free(&fb);
}

static void test_raw(void)
{
    csvnc_pixfmt f;
    csvnc_fb fb;
    uint8_t buf[4096];
    size_t n;
    int x, y;

    TCHECK_EQ(csvnc_fb_init(&fb, 10, 6), 0);
    for (y = 0; y < 6; y++)
        for (x = 0; x < 10; x++)
            fb.px[y * 10 + x] = ((uint32_t)x << 16) | ((uint32_t)y << 8) | 0x77;
    csvnc_pixfmt_native(&f);
    n = csvnc_encode_raw(&f, &fb, 2, 1, 3, 4, buf, sizeof(buf));
    TCHECK_EQ(n, 3 * 4 * 4);
    for (y = 0; y < 4; y++)
        for (x = 0; x < 3; x++) {
            const uint8_t *p = buf + (y * 3 + x) * 4;

            TCHECK_EQ(p[0], 0x77);
            TCHECK_EQ(p[1], y + 1);
            TCHECK_EQ(p[2], x + 2);
            TCHECK_EQ(p[3], 0);
        }
    make_fmt(&f, 3);   /* 565 */
    n = csvnc_encode_raw(&f, &fb, 0, 0, 10, 6, buf, sizeof(buf));
    TCHECK_EQ(n, 10 * 6 * 2);
    TCHECK_EQ(csvnc_pix_get(&f, buf + (5 * 10 + 9) * 2),
              csvnc_pix_translate(&f, fb.px[5 * 10 + 9]));
    TCHECK_EQ(csvnc_encode_raw(&f, &fb, 0, 0, 10, 6, buf, 119), 0);
    TCHECK_EQ(csvnc_encode_raw(&f, &fb, 0, 0, 10, 6, buf, 120), 120);
    csvnc_fb_free(&fb);
}

static void test_encode_rect(void)
{
    csvnc_encoder e;
    csvnc_pixfmt f;
    csvnc_fb fb;
    uint8_t buf[8192];
    size_t n, cap;
    int i;

    make_fmt(&f, 1);
    TCHECK_EQ(csvnc_fb_init(&fb, 40, 20), 0);
    for (i = 0; i < 40 * 20; i++)
        fb.px[i] = 0x00123456;
    csvnc_encoder_init(&e);
    TCHECK_EQ(e.enc, CSVNC_ENC_RAW);
    cap = csvnc_encode_max_size(&e, &f, 40, 20);
    TCHECK_EQ(cap, 12 + 40 * 20 * 4);
    n = csvnc_encode_rect(&e, &f, &fb, 0, 0, 40, 20, buf, sizeof(buf));
    TCHECK_EQ(n, cap);
    TCHECK_EQ(csvnc_get_u16(buf), 0);
    TCHECK_EQ(csvnc_get_u16(buf + 2), 0);
    TCHECK_EQ(csvnc_get_u16(buf + 4), 40);
    TCHECK_EQ(csvnc_get_u16(buf + 6), 20);
    TCHECK_EQ(csvnc_get_u32(buf + 8), 0);
    TCHECK_EQ(buf[12], 0x12);          /* R first in noVNC's format */
    TCHECK_EQ(buf[14], 0x56);

    csvnc_encoder_select(&e, CSVNC_ENC_HEXTILE);
    TCHECK_EQ(e.enc, CSVNC_ENC_HEXTILE);
    cap = csvnc_encode_max_size(&e, &f, 40, 20);
    TCHECK_EQ(cap, 12 + 40 * 20 * 4 + 3 * 2);
    n = csvnc_encode_rect(&e, &f, &fb, 8, 4, 32, 16, buf, sizeof(buf));
    TCHECK_EQ(n, 12 + 6);
    TCHECK_EQ(csvnc_get_u16(buf), 8);
    TCHECK_EQ(csvnc_get_u16(buf + 2), 4);
    TCHECK_EQ(csvnc_get_u16(buf + 4), 32);
    TCHECK_EQ(csvnc_get_u16(buf + 6), 16);
    TCHECK_EQ(csvnc_get_u32(buf + 8), 5);
    TCHECK_EQ(buf[12], 2);
    TCHECK_EQ(buf[17], 0);
    /* unsupported encodings fall back to Raw */
    csvnc_encoder_select(&e, CSVNC_ENC_ZRLE);
    TCHECK_EQ(e.enc, CSVNC_ENC_ZRLE);
    csvnc_encoder_select(&e, 7);
    TCHECK_EQ(e.enc, CSVNC_ENC_RAW);
    /* out-of-frame or empty rectangles are refused */
    TCHECK_EQ(csvnc_encode_rect(&e, &f, &fb, 0, 0, 41, 20, buf, sizeof(buf)), 0);
    TCHECK_EQ(csvnc_encode_rect(&e, &f, &fb, 1, 0, 40, 20, buf, sizeof(buf)), 0);
    TCHECK_EQ(csvnc_encode_rect(&e, &f, &fb, -1, 0, 4, 4, buf, sizeof(buf)), 0);
    TCHECK_EQ(csvnc_encode_rect(&e, &f, &fb, 0, 0, 0, 4, buf, sizeof(buf)), 0);
    TCHECK_EQ(csvnc_encode_rect(&e, &f, &fb, 0, 0, 4, 4, buf, 11), 0);
    TCHECK_EQ(csvnc_encode_rect(&e, &f, &fb, 0, 0, 4, 4, buf, 12 + 63), 0);
    TCHECK_EQ(csvnc_encode_rect(&e, &f, &fb, 0, 0, 4, 4, buf, 12 + 64), 12 + 64);
    csvnc_encoder_free(&e);
    csvnc_fb_free(&fb);

    csvnc_put_update_header(buf, 3);
    TCHECK_EQ(buf[0], 0);
    TCHECK_EQ(buf[1], 0);
    TCHECK_EQ(csvnc_get_u16(buf + 2), 3);
    csvnc_put_rect_header(buf, 1, 2, 3, 4, CSVNC_PSEUDO_LAST_RECT);
    TCHECK_EQ(csvnc_get_u16(buf + 4), 3);
    TCHECK_EQ((int32_t)csvnc_get_u32(buf + 8), -224);
}

/* ---- ZRLE ---- */

static const struct {
    int bpp, depth, be, rmax, gmax, bmax, rs, gs, bs, cpixel;
} zformats[] = {
    { 32, 24, 0, 255, 255, 255, 16, 8, 0, 3 },    /* native: LS 3 bytes */
    { 32, 24, 0, 255, 255, 255, 0, 8, 16, 3 },    /* noVNC */
    { 32, 24, 1, 255, 255, 255, 16, 8, 0, 3 },    /* big-endian, LS 3 -> offset 1 */
    { 32, 24, 1, 255, 255, 255, 24, 16, 8, 3 },   /* big-endian, MS 3 */
    { 32, 24, 0, 255, 255, 255, 24, 16, 8, 3 },   /* LE with MS 3 -> offset 1 */
    { 32, 32, 0, 255, 255, 255, 16, 8, 0, 4 },    /* depth 32: 4 bytes */
    { 32, 24, 0, 255, 255, 255, 20, 10, 0, 4 },   /* straddles: 4 bytes */
    { 16, 16, 0, 31, 63, 31, 11, 5, 0, 2 },
    { 16, 16, 1, 31, 63, 31, 11, 5, 0, 2 },
    { 8, 8, 0, 7, 7, 3, 5, 2, 0, 1 },
};
#define NZFORMATS (sizeof(zformats) / sizeof(zformats[0]))

static void make_zfmt(csvnc_pixfmt *f, size_t i)
{
    uint8_t w[16];

    memset(w, 0, 16);
    w[0] = (uint8_t)zformats[i].bpp;
    w[1] = (uint8_t)zformats[i].depth;
    w[2] = (uint8_t)zformats[i].be;
    w[3] = 1;
    csvnc_put_u16(w + 4, (unsigned)zformats[i].rmax);
    csvnc_put_u16(w + 6, (unsigned)zformats[i].gmax);
    csvnc_put_u16(w + 8, (unsigned)zformats[i].bmax);
    w[10] = (uint8_t)zformats[i].rs;
    w[11] = (uint8_t)zformats[i].gs;
    w[12] = (uint8_t)zformats[i].bs;
    TCHECK_EQ(csvnc_pixfmt_parse(f, w), 0);
    TCHECK_EQ(csvnc_zrle_cpixel_bytes(f), zformats[i].cpixel);
}

/* Encode (x,y,w,h) with the encoder's ZRLE stream, decode with the
 * matching reference stream, compare.  Returns the encoded size. */
static size_t zrle_round_trip(csvnc_encoder *e, zrle_ref *z, const csvnc_pixfmt *f,
                              const csvnc_fb *fb, int x, int y, int w, int h)
{
    size_t cap = csvnc_zrle_max_size(f, w, h);
    uint8_t *buf = malloc(cap + 16);
    uint32_t *dec = malloc((size_t)w * h * sizeof(uint32_t));
    size_t n;
    long used;
    int i, j, bad = 0;

    memset(buf, 0xa5, cap + 16);
    n = csvnc_encode_zrle(e, f, fb, x, y, w, h, buf, cap);
    TCHECK(n > 0);
    TCHECK(n <= cap);
    for (i = 0; i < 16; i++)
        TCHECK_EQ(buf[cap + i], 0xa5);
    used = zrle_ref_decode(z, f, buf, n, w, h, dec);
    TCHECK_EQ(used, (long)n);
    if (used == (long)n) {
        for (j = 0; j < h && !bad; j++)
            for (i = 0; i < w; i++) {
                uint32_t expect = csvnc_pix_translate(f, fb->px[(y + j) * fb->width + x + i]);

                if (dec[j * w + i] != expect) {
                    TFAIL("zrle pixel (%d,%d) of %dx%d rect at (%d,%d): %x != %x",
                          i, j, w, h, x, y, dec[j * w + i], expect);
                    bad = 1;
                    break;
                }
            }
    }
    free(buf);
    free(dec);
    return n;
}

static void test_zrle_round_trip(void)
{
    static const struct { int w, h; } sizes[] = {
        { 64, 64 }, { 1, 1 }, { 65, 64 }, { 64, 65 }, { 37, 23 },
        { 130, 3 }, { 3, 130 }, { 63, 63 }, { 200, 100 },
    };
    size_t fi, si;
    int kind;

    for (fi = 0; fi < NZFORMATS; fi++) {
        csvnc_pixfmt f;
        csvnc_encoder e;
        zrle_ref z;

        make_zfmt(&f, fi);
        /* one stream for the whole sequence, as one client would */
        csvnc_encoder_init(&e);
        csvnc_encoder_select(&e, CSVNC_ENC_ZRLE);
        zrle_ref_init(&z);
        for (kind = 0; kind < FR_N; kind++) {
            for (si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
                csvnc_fb fb;
                int w = sizes[si].w, h = sizes[si].h;

                TCHECK_EQ(csvnc_fb_init(&fb, w + 7, h + 5), 0);
                fill_frame(&fb, (frame_kind)kind);
                zrle_round_trip(&e, &z, &f, &fb, 0, 0, fb.width, fb.height);
                zrle_round_trip(&e, &z, &f, &fb, 3, 2, w, h);
                zrle_round_trip(&e, &z, &f, &fb, 7, 5, w, h);
                csvnc_fb_free(&fb);
            }
        }
        TCHECK(e.zrle != NULL);
        csvnc_encoder_free(&e);
        TCHECK(e.zrle == NULL);
        zrle_ref_free(&z);
    }
}

static void test_zrle_subencodings(void)
{
    /* each tile kind picks the expected subencoding and beats raw */
    csvnc_pixfmt f;
    csvnc_encoder e;
    zrle_ref z;
    csvnc_fb fb;
    uint8_t buf[65536];
    size_t n;
    int i;

    make_zfmt(&f, 1);   /* noVNC: 3-byte CPIXEL */
    csvnc_encoder_init(&e);
    csvnc_encoder_select(&e, CSVNC_ENC_ZRLE);
    zrle_ref_init(&z);
    TCHECK_EQ(csvnc_fb_init(&fb, 64, 64), 0);

    /* solid: a whole tile is a few bytes */
    for (i = 0; i < 64 * 64; i++)
        fb.px[i] = 0x336699;
    n = zrle_round_trip(&e, &z, &f, &fb, 0, 0, 64, 64);
    TCHECK(n < 4 + 16);
    TCHECK_EQ(z.buf[0], 1);

    /* two colours in long runs: palette RLE (130) */
    for (i = 0; i < 64 * 64; i++)
        fb.px[i] = (i / 64) % 8 < 4 ? 0x336699 : 0xffffff;
    n = zrle_round_trip(&e, &z, &f, &fb, 0, 0, 64, 64);
    TCHECK_EQ(z.buf[0], 130);
    TCHECK(z.len < 64);

    /* two colours alternating every pixel: packed palette, 1 bit */
    for (i = 0; i < 64 * 64; i++)
        fb.px[i] = ((i % 64) + (i / 64)) & 1 ? 0xffffff : 0;
    n = zrle_round_trip(&e, &z, &f, &fb, 0, 0, 64, 64);
    TCHECK_EQ(z.buf[0], 2);
    TCHECK_EQ(z.len, 1 + 2 * 3 + 64 * 8);

    /* 16 colours alternating: packed palette, 4 bits */
    for (i = 0; i < 64 * 64; i++)
        fb.px[i] = 0x111111u * (uint32_t)((i % 64 + i / 64) % 16);
    n = zrle_round_trip(&e, &z, &f, &fb, 0, 0, 64, 64);
    TCHECK_EQ(z.buf[0], 16);
    TCHECK_EQ(z.len, 1 + 16 * 3 + 64 * 32);

    /* many colours in long runs: plain RLE (128) */
    for (i = 0; i < 64 * 64; i++)
        fb.px[i] = 0x010203u * (uint32_t)(i / 32);
    n = zrle_round_trip(&e, &z, &f, &fb, 0, 0, 64, 64);
    TCHECK_EQ(z.buf[0], 128);
    TCHECK_EQ(z.len, 1 + 128 * (3 + 1));

    /* noise: raw */
    fill_frame(&fb, FR_NOISE);
    n = zrle_round_trip(&e, &z, &f, &fb, 0, 0, 64, 64);
    TCHECK_EQ(z.buf[0], 0);
    TCHECK_EQ(z.len, 1 + 64 * 64 * 3);

    /* a run longer than 255 encodes its length in continuation bytes */
    for (i = 0; i < 64 * 64; i++)
        fb.px[i] = i < 3000 ? 0x102030 : 0x405060;
    n = zrle_round_trip(&e, &z, &f, &fb, 0, 0, 64, 64);
    /* two runs: plain RLE (pixel + length) beats a palette here:
     * run 3000 = 3 + (2999/255=11)+1 bytes, run 1096 = 3 + 5 */
    TCHECK_EQ(z.buf[0], 128);
    TCHECK_EQ(z.len, 1 + (3 + 12) + (3 + 5));

    /* buffer too small: refused, nothing written past cap */
    memset(buf, 0xee, sizeof(buf));
    n = csvnc_encode_zrle(&e, &f, &fb, 0, 0, 64, 64, buf, 3);
    TCHECK_EQ(n, 0);
    TCHECK_EQ(buf[3], 0xee);

    csvnc_fb_free(&fb);
    csvnc_encoder_free(&e);
    zrle_ref_free(&z);
}

static void test_zrle_full_frame(void)
{
    /* the console frame: flat, dotted, noise; sizes bounded by
     * csvnc_zrle_max_size in the worst case */
    csvnc_pixfmt f;
    csvnc_encoder e;
    zrle_ref z;
    csvnc_fb fb;
    size_t n;

    make_zfmt(&f, 1);
    csvnc_encoder_init(&e);
    csvnc_encoder_select(&e, CSVNC_ENC_ZRLE);
    zrle_ref_init(&z);
    TCHECK_EQ(csvnc_fb_init(&fb, 640, 480), 0);
    fill_frame(&fb, FR_FLAT);
    n = zrle_round_trip(&e, &z, &f, &fb, 0, 0, 640, 480);
    TCHECK(n < 200);
    fill_frame(&fb, FR_DOTS);
    n = zrle_round_trip(&e, &z, &f, &fb, 0, 0, 640, 480);
    TCHECK(n < 640 * 480 * 3 / 20);
    fill_frame(&fb, FR_NOISE);
    n = zrle_round_trip(&e, &z, &f, &fb, 0, 0, 640, 480);
    TCHECK(n <= csvnc_zrle_max_size(&f, 640, 480));
    TCHECK(n > 640 * 480 * 3 * 9 / 10);
    /* the same through the dispatcher, header included */
    {
        size_t cap = csvnc_encode_max_size(&e, &f, 640, 64);
        uint8_t *buf = malloc(cap);

        n = csvnc_encode_rect(&e, &f, &fb, 0, 0, 640, 64, buf, cap);
        TCHECK(n > 12);
        TCHECK(n <= cap);
        TCHECK_EQ((int32_t)csvnc_get_u32(buf + 8), CSVNC_ENC_ZRLE);
        free(buf);
    }
    csvnc_fb_free(&fb);
    csvnc_encoder_free(&e);
    zrle_ref_free(&z);
}

TEST_MAIN(test_hextile_round_trip, test_hextile_full_frame, test_hextile_costs,
          test_raw, test_encode_rect, test_zrle_round_trip,
          test_zrle_subencodings, test_zrle_full_frame)
