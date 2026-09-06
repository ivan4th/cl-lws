/* Pixel format parsing/validation and shadow -> client conversion
 * against hand-computed pixels; the shadow framebuffer blit. */

#include "../csvnc-private.h"
#include "test-util.h"

static void wire_fmt(uint8_t w[16], int bpp, int depth, int be, int tc,
                     int rmax, int gmax, int bmax, int rs, int gs, int bs)
{
    memset(w, 0, 16);
    w[0] = (uint8_t)bpp;
    w[1] = (uint8_t)depth;
    w[2] = (uint8_t)be;
    w[3] = (uint8_t)tc;
    csvnc_put_u16(w + 4, (unsigned)rmax);
    csvnc_put_u16(w + 6, (unsigned)gmax);
    csvnc_put_u16(w + 8, (unsigned)bmax);
    w[10] = (uint8_t)rs;
    w[11] = (uint8_t)gs;
    w[12] = (uint8_t)bs;
}

static void test_native(void)
{
    csvnc_pixfmt f;
    uint8_t w[16], expect[16];

    csvnc_pixfmt_native(&f);
    TCHECK_EQ(f.bpp, 32);
    TCHECK_EQ(f.depth, 24);
    TCHECK_EQ(f.big_endian, 0);
    TCHECK_EQ(f.true_colour, 1);
    TCHECK_EQ(f.bytespp, 4);
    TCHECK_EQ(csvnc_pix_translate(&f, 0x00123456), 0x00123456);
    TCHECK_EQ(csvnc_pix_translate(&f, 0xff123456), 0x00123456);   /* X byte ignored */
    csvnc_pixfmt_write(&f, w);
    wire_fmt(expect, 32, 24, 0, 1, 255, 255, 255, 16, 8, 0);
    TCHECK(memcmp(w, expect, 16) == 0);
    /* round trip through parse */
    TCHECK_EQ(csvnc_pixfmt_parse(&f, w), 0);
    TCHECK_EQ(csvnc_pix_translate(&f, 0x00abcdef), 0x00abcdef);
}

static void test_novnc_format(void)
{
    /* noVNC: 32 bpp, depth 24, little-endian, R shift 0, G 8, B 16 */
    csvnc_pixfmt f;
    uint8_t w[16], out[8];

    wire_fmt(w, 32, 24, 0, 1, 255, 255, 255, 0, 8, 16);
    TCHECK_EQ(csvnc_pixfmt_parse(&f, w), 0);
    TCHECK_EQ(f.bytespp, 4);
    TCHECK_EQ(csvnc_pix_translate(&f, 0x00ff0000), 0x000000ff);   /* red */
    TCHECK_EQ(csvnc_pix_translate(&f, 0x0000ff00), 0x0000ff00);   /* green */
    TCHECK_EQ(csvnc_pix_translate(&f, 0x000000ff), 0x00ff0000);   /* blue */
    TCHECK_EQ(csvnc_pix_translate(&f, 0x00123456), 0x00563412);
    /* on the wire: bytes R, G, B, 0 */
    csvnc_pix_put(&f, out, csvnc_pix_translate(&f, 0x00123456));
    TCHECK_EQ(out[0], 0x12);
    TCHECK_EQ(out[1], 0x34);
    TCHECK_EQ(out[2], 0x56);
    TCHECK_EQ(out[3], 0x00);
    TCHECK_EQ(csvnc_pix_get(&f, out), 0x00563412);
    {
        uint32_t src[2] = { 0x00ff8000, 0x00000001 };
        uint8_t *end = csvnc_pix_put_row(&f, out, src, 2);

        TCHECK_EQ(end - out, 8);
        TCHECK_EQ(out[0], 0xff);
        TCHECK_EQ(out[1], 0x80);
        TCHECK_EQ(out[2], 0x00);
        TCHECK_EQ(out[3], 0x00);
        TCHECK_EQ(out[4], 0x00);
        TCHECK_EQ(out[5], 0x00);
        TCHECK_EQ(out[6], 0x01);
        TCHECK_EQ(out[7], 0x00);
    }
}

static void test_rgb565(void)
{
    csvnc_pixfmt f;
    uint8_t w[16], out[4];
    uint32_t src[2];

    wire_fmt(w, 16, 16, 0, 1, 31, 63, 31, 11, 5, 0);
    TCHECK_EQ(csvnc_pixfmt_parse(&f, w), 0);
    TCHECK_EQ(f.bytespp, 2);
    TCHECK_EQ(csvnc_pix_translate(&f, 0x00ff0000), 0xf800);
    TCHECK_EQ(csvnc_pix_translate(&f, 0x0000ff00), 0x07e0);
    TCHECK_EQ(csvnc_pix_translate(&f, 0x000000ff), 0x001f);
    TCHECK_EQ(csvnc_pix_translate(&f, 0x00ffffff), 0xffff);
    TCHECK_EQ(csvnc_pix_translate(&f, 0), 0);
    /* 0x808080: r = (128*31+127)/255 = 16, g = (128*63+127)/255 = 32, b = 16 */
    TCHECK_EQ(csvnc_pix_translate(&f, 0x00808080), (16u << 11) | (32u << 5) | 16u);
    TCHECK_EQ(csvnc_pix_translate(&f, 0x00808080), 0x8410);
    /* rounding: 8 -> 1 in 5 bits (8*31/255 = 0.97), 4 -> 0 */
    TCHECK_EQ(csvnc_pix_translate(&f, 0x00080000), 1u << 11);
    TCHECK_EQ(csvnc_pix_translate(&f, 0x00040000), 0);
    /* little-endian on the wire */
    src[0] = 0x00808080;
    src[1] = 0x00ff0000;
    csvnc_pix_put_row(&f, out, src, 2);
    TCHECK_EQ(out[0], 0x10);
    TCHECK_EQ(out[1], 0x84);
    TCHECK_EQ(out[2], 0x00);
    TCHECK_EQ(out[3], 0xf8);
    TCHECK_EQ(csvnc_pix_get(&f, out), 0x8410);
    /* the same format big-endian */
    wire_fmt(w, 16, 16, 1, 1, 31, 63, 31, 11, 5, 0);
    TCHECK_EQ(csvnc_pixfmt_parse(&f, w), 0);
    csvnc_pix_put_row(&f, out, src, 2);
    TCHECK_EQ(out[0], 0x84);
    TCHECK_EQ(out[1], 0x10);
    TCHECK_EQ(out[2], 0xf8);
    TCHECK_EQ(out[3], 0x00);
    TCHECK_EQ(csvnc_pix_get(&f, out), 0x8410);
}

static void test_bgr233(void)
{
    /* 8 bpp: B in the top 2 bits? No: the classic RFB 8-bit format is
     * R max 7 shift 0, G max 7 shift 3, B max 3 shift 6 (BGR233).  Both
     * that and RGB332 (R shift 5, G 2, B 0) must work. */
    csvnc_pixfmt f;
    uint8_t w[16], out[3];
    uint32_t src[3];

    wire_fmt(w, 8, 8, 0, 1, 7, 7, 3, 5, 2, 0);   /* RGB332 */
    TCHECK_EQ(csvnc_pixfmt_parse(&f, w), 0);
    TCHECK_EQ(f.bytespp, 1);
    TCHECK_EQ(csvnc_pix_translate(&f, 0x00ff0000), 0xe0);
    TCHECK_EQ(csvnc_pix_translate(&f, 0x0000ff00), 0x1c);
    TCHECK_EQ(csvnc_pix_translate(&f, 0x000000ff), 0x03);
    TCHECK_EQ(csvnc_pix_translate(&f, 0x00ffffff), 0xff);
    /* 0x808080: r = (128*7+127)/255 = 4, g = 4, b = (128*3+127)/255 = 2 */
    TCHECK_EQ(csvnc_pix_translate(&f, 0x00808080), (4u << 5) | (4u << 2) | 2u);
    TCHECK_EQ(csvnc_pix_translate(&f, 0x00808080), 0x92);
    src[0] = 0x00ff0000;
    src[1] = 0x00808080;
    src[2] = 0x00000000;
    csvnc_pix_put_row(&f, out, src, 3);
    TCHECK_EQ(out[0], 0xe0);
    TCHECK_EQ(out[1], 0x92);
    TCHECK_EQ(out[2], 0x00);
    TCHECK_EQ(csvnc_pix_get(&f, out + 1), 0x92);

    wire_fmt(w, 8, 8, 0, 1, 7, 7, 3, 0, 3, 6);   /* BGR233 */
    TCHECK_EQ(csvnc_pixfmt_parse(&f, w), 0);
    TCHECK_EQ(csvnc_pix_translate(&f, 0x00ff0000), 0x07);
    TCHECK_EQ(csvnc_pix_translate(&f, 0x0000ff00), 0x38);
    TCHECK_EQ(csvnc_pix_translate(&f, 0x000000ff), 0xc0);
    /* big-endian flag is irrelevant at 8 bpp but must be accepted */
    wire_fmt(w, 8, 6, 1, 1, 3, 3, 3, 4, 2, 0);
    TCHECK_EQ(csvnc_pixfmt_parse(&f, w), 0);
    TCHECK_EQ(csvnc_pix_translate(&f, 0x00ffffff), 0x3f);
}

static void test_big_endian_32(void)
{
    /* 32 bpp big-endian XRGB: bytes X, R, G, B on the wire */
    csvnc_pixfmt f;
    uint8_t w[16], out[8];
    uint32_t src[2] = { 0x00123456, 0x00ff0000 };

    wire_fmt(w, 32, 24, 1, 1, 255, 255, 255, 16, 8, 0);
    TCHECK_EQ(csvnc_pixfmt_parse(&f, w), 0);
    TCHECK_EQ(csvnc_pix_translate(&f, 0x00123456), 0x00123456);
    csvnc_pix_put_row(&f, out, src, 2);
    TCHECK_EQ(out[0], 0x00);
    TCHECK_EQ(out[1], 0x12);
    TCHECK_EQ(out[2], 0x34);
    TCHECK_EQ(out[3], 0x56);
    TCHECK_EQ(out[4], 0x00);
    TCHECK_EQ(out[5], 0xff);
    TCHECK_EQ(out[6], 0x00);
    TCHECK_EQ(out[7], 0x00);
    TCHECK_EQ(csvnc_pix_get(&f, out), 0x00123456);
    /* big-endian with the channels at the top: R shift 24 */
    wire_fmt(w, 32, 24, 1, 1, 255, 255, 255, 24, 16, 8);
    TCHECK_EQ(csvnc_pixfmt_parse(&f, w), 0);
    TCHECK_EQ(csvnc_pix_translate(&f, 0x00123456), 0x12345600);
    csvnc_pix_put(&f, out, csvnc_pix_translate(&f, 0x00123456));
    TCHECK_EQ(out[0], 0x12);
    TCHECK_EQ(out[1], 0x34);
    TCHECK_EQ(out[2], 0x56);
    TCHECK_EQ(out[3], 0x00);
    /* and the same little-endian: bytes 0, B, G, R */
    wire_fmt(w, 32, 24, 0, 1, 255, 255, 255, 24, 16, 8);
    TCHECK_EQ(csvnc_pixfmt_parse(&f, w), 0);
    csvnc_pix_put(&f, out, csvnc_pix_translate(&f, 0x00123456));
    TCHECK_EQ(out[0], 0x00);
    TCHECK_EQ(out[1], 0x56);
    TCHECK_EQ(out[2], 0x34);
    TCHECK_EQ(out[3], 0x12);
    TCHECK_EQ(csvnc_pix_get(&f, out), 0x12345600);
}

static void test_rejected(void)
{
    csvnc_pixfmt f;
    uint8_t w[16];

    csvnc_pixfmt_native(&f);
    /* colour map */
    wire_fmt(w, 8, 8, 0, 0, 7, 7, 3, 5, 2, 0);
    TCHECK_EQ(csvnc_pixfmt_parse(&f, w), -1);
    /* odd bpp */
    wire_fmt(w, 24, 24, 0, 1, 255, 255, 255, 16, 8, 0);
    TCHECK_EQ(csvnc_pixfmt_parse(&f, w), -1);
    wire_fmt(w, 0, 0, 0, 1, 255, 255, 255, 16, 8, 0);
    TCHECK_EQ(csvnc_pixfmt_parse(&f, w), -1);
    wire_fmt(w, 64, 24, 0, 1, 255, 255, 255, 16, 8, 0);
    TCHECK_EQ(csvnc_pixfmt_parse(&f, w), -1);
    /* depth out of range */
    wire_fmt(w, 16, 24, 0, 1, 31, 63, 31, 11, 5, 0);
    TCHECK_EQ(csvnc_pixfmt_parse(&f, w), -1);
    wire_fmt(w, 32, 0, 0, 1, 255, 255, 255, 16, 8, 0);
    TCHECK_EQ(csvnc_pixfmt_parse(&f, w), -1);
    /* a channel that does not fit */
    wire_fmt(w, 32, 24, 0, 1, 255, 255, 255, 25, 8, 0);
    TCHECK_EQ(csvnc_pixfmt_parse(&f, w), -1);
    wire_fmt(w, 16, 16, 0, 1, 31, 63, 31, 11, 5, 12);
    TCHECK_EQ(csvnc_pixfmt_parse(&f, w), -1);
    wire_fmt(w, 16, 16, 0, 1, 65535, 63, 31, 1, 5, 0);
    TCHECK_EQ(csvnc_pixfmt_parse(&f, w), -1);
    wire_fmt(w, 8, 8, 0, 1, 255, 7, 3, 1, 2, 0);
    TCHECK_EQ(csvnc_pixfmt_parse(&f, w), -1);
    wire_fmt(w, 32, 24, 0, 1, 255, 255, 255, 32, 8, 0);
    TCHECK_EQ(csvnc_pixfmt_parse(&f, w), -1);
    /* zero max */
    wire_fmt(w, 32, 24, 0, 1, 0, 255, 255, 16, 8, 0);
    TCHECK_EQ(csvnc_pixfmt_parse(&f, w), -1);
    /* rejection leaves the format untouched */
    TCHECK_EQ(f.bpp, 32);
    TCHECK_EQ(f.rshift, 16);
    TCHECK_EQ(csvnc_pix_translate(&f, 0x00123456), 0x00123456);
    /* overlapping channels are legal as far as the spec goes: accept */
    wire_fmt(w, 32, 24, 0, 1, 255, 255, 255, 0, 0, 0);
    TCHECK_EQ(csvnc_pixfmt_parse(&f, w), 0);
    /* odd maxima that are not 2^n-1 fit by bit count */
    wire_fmt(w, 16, 16, 0, 1, 20, 40, 20, 11, 5, 0);
    TCHECK_EQ(csvnc_pixfmt_parse(&f, w), 0);
    TCHECK_EQ(csvnc_pix_translate(&f, 0x00ffffff), (20u << 11) | (40u << 5) | 20u);
}

static void test_fb_blit(void)
{
    csvnc_fb fb;
    csvnc_region dirty;
    uint32_t src[3 * 4];
    uint16_t src565[2 * 2];
    int i;

    TCHECK_EQ(csvnc_fb_init(&fb, 8, 6), 0);
    TCHECK_EQ(fb.width, 8);
    TCHECK_EQ(fb.height, 6);
    for (i = 0; i < 48; i++)
        TCHECK_EQ(fb.px[i], 0);
    csvnc_region_init(&dirty, 8, 6);
    for (i = 0; i < 12; i++)
        src[i] = 0x00100000u * (uint32_t)(i + 1);
    /* 3 wide x 4 tall block from a 3-pixel-stride source at (2,1) */
    TCHECK_EQ(csvnc_fb_blit(&fb, 2, 1, 3, 4, src, 12, CSVNC_SRC_XRGB8888, &dirty), 4);
    TCHECK_EQ(fb.px[1 * 8 + 2], 0x00100000);
    TCHECK_EQ(fb.px[1 * 8 + 4], 0x00300000);
    TCHECK_EQ(fb.px[4 * 8 + 4], 0x00c00000);
    TCHECK_EQ(fb.px[1 * 8 + 1], 0);
    TCHECK_EQ(fb.px[1 * 8 + 5], 0);
    TCHECK_EQ(fb.px[5 * 8 + 2], 0);
    /* the whole block changed: exactly it is dirty */
    TCHECK_EQ(dirty.n, 1);
    TCHECK_EQ(dirty.r[0].x, 2);
    TCHECK_EQ(dirty.r[0].y, 1);
    TCHECK_EQ(dirty.r[0].w, 3);
    TCHECK_EQ(dirty.r[0].h, 4);
    /* the same block again: nothing changes, nothing is dirty; a
     * different X byte does not count as a change */
    csvnc_region_clear(&dirty);
    for (i = 0; i < 12; i++)
        src[i] |= 0xff000000u;
    TCHECK_EQ(csvnc_fb_blit(&fb, 2, 1, 3, 4, src, 12, CSVNC_SRC_XRGB8888, &dirty), 0);
    TCHECK(csvnc_region_empty(&dirty));
    TCHECK_EQ(fb.px[1 * 8 + 2], 0x00100000);
    /* one pixel changes: a 1x1 dirty rectangle */
    src[7] = 0x00000001;   /* row 2, col 1 of the block = (3,3) */
    TCHECK_EQ(csvnc_fb_blit(&fb, 2, 1, 3, 4, src, 12, CSVNC_SRC_XRGB8888, &dirty), 1);
    TCHECK_EQ(dirty.n, 1);
    TCHECK_EQ(dirty.r[0].x, 3);
    TCHECK_EQ(dirty.r[0].y, 3);
    TCHECK_EQ(dirty.r[0].w, 1);
    TCHECK_EQ(dirty.r[0].h, 1);
    TCHECK_EQ(fb.px[3 * 8 + 3], 1);
    /* two changed pixels on different rows: their bounding box */
    csvnc_region_clear(&dirty);
    src[0] = 0x00000002;   /* (2,1) */
    src[11] = 0x00000003;  /* (4,4) */
    TCHECK_EQ(csvnc_fb_blit(&fb, 2, 1, 3, 4, src, 12, CSVNC_SRC_XRGB8888, &dirty), 2);
    TCHECK_EQ(dirty.n, 1);
    TCHECK_EQ(dirty.r[0].x, 2);
    TCHECK_EQ(dirty.r[0].y, 1);
    TCHECK_EQ(dirty.r[0].w, 3);
    TCHECK_EQ(dirty.r[0].h, 4);
    /* clipping on the right/bottom: source rows stay aligned */
    csvnc_region_clear(&dirty);
    for (i = 0; i < 12; i++)
        src[i] = 0x00100000u * (uint32_t)(i + 1);
    TCHECK_EQ(csvnc_fb_blit(&fb, 6, 4, 3, 4, src, 12, CSVNC_SRC_XRGB8888, &dirty), 2);
    TCHECK_EQ(fb.px[4 * 8 + 6], 0x00100000);
    TCHECK_EQ(fb.px[4 * 8 + 7], 0x00200000);
    TCHECK_EQ(fb.px[5 * 8 + 6], 0x00400000);
    TCHECK_EQ(fb.px[5 * 8 + 7], 0x00500000);
    TCHECK_EQ(dirty.n, 1);
    TCHECK_EQ(dirty.r[0].x, 6);
    TCHECK_EQ(dirty.r[0].y, 4);
    TCHECK_EQ(dirty.r[0].w, 2);
    TCHECK_EQ(dirty.r[0].h, 2);
    /* clipping at the top/left skips source pixels */
    csvnc_region_clear(&dirty);
    TCHECK_EQ(csvnc_fb_blit(&fb, -1, -2, 3, 4, src, 12, CSVNC_SRC_XRGB8888, &dirty), 2);
    TCHECK_EQ(fb.px[0], 0x00800000);   /* src row 2, col 1 */
    TCHECK_EQ(fb.px[1], 0x00900000);
    TCHECK_EQ(fb.px[8], 0x00b00000);
    TCHECK_EQ(fb.px[9], 0x00c00000);
    TCHECK_EQ(dirty.n, 1);
    TCHECK_EQ(dirty.r[0].x, 0);
    TCHECK_EQ(dirty.r[0].y, 0);
    TCHECK_EQ(dirty.r[0].w, 2);
    TCHECK_EQ(dirty.r[0].h, 2);
    /* entirely outside, or no dirty region wanted */
    csvnc_region_clear(&dirty);
    TCHECK_EQ(csvnc_fb_blit(&fb, 8, 0, 2, 2, src, 12, CSVNC_SRC_XRGB8888, &dirty), 0);
    TCHECK_EQ(csvnc_fb_blit(&fb, 0, 6, 2, 2, src, 12, CSVNC_SRC_XRGB8888, &dirty), 0);
    TCHECK(csvnc_region_empty(&dirty));
    TCHECK_EQ(csvnc_fb_blit(&fb, 0, 0, 2, 2, src, 8, CSVNC_SRC_XRGB8888, NULL), 2);
    TCHECK_EQ(fb.px[0], 0x00100000);
    /* RGB565 expansion with bit replication: 0xf800 -> ff0000,
     * 0x07e0 -> 00ff00, 0x001f -> 0000ff, 0x8410 -> 848284 */
    src565[0] = 0xf800;
    src565[1] = 0x07e0;
    src565[2] = 0x001f;
    src565[3] = 0x8410;
    TCHECK_EQ(csvnc_fb_blit(&fb, 0, 0, 2, 2, src565, 4, CSVNC_SRC_RGB565, &dirty), 2);
    TCHECK_EQ(fb.px[0], 0x00ff0000);
    TCHECK_EQ(fb.px[1], 0x0000ff00);
    TCHECK_EQ(fb.px[8], 0x000000ff);
    TCHECK_EQ(fb.px[9], 0x00848284);
    TCHECK_EQ(dirty.n, 1);
    TCHECK_EQ(dirty.r[0].w, 2);
    TCHECK_EQ(dirty.r[0].h, 2);
    csvnc_fb_free(&fb);
    TCHECK(fb.px == NULL);
    /* bad geometry */
    TCHECK_EQ(csvnc_fb_init(&fb, 0, 10), -1);
    TCHECK_EQ(csvnc_fb_init(&fb, 10, -1), -1);
    TCHECK_EQ(csvnc_fb_init(&fb, 100000, 100000), -1);
}

static void test_fb_blit_bands(void)
{
    /* changes in different 16-row bands become separate rectangles
     * (and adjacent, waste-free ones merge in the region) */
    csvnc_fb fb;
    csvnc_region dirty;
    uint32_t *src;
    int i;

    TCHECK_EQ(csvnc_fb_init(&fb, 64, 48), 0);
    csvnc_region_init(&dirty, 64, 48);
    src = calloc(64 * 48, 4);
    src[5 * 64 + 10] = 1;        /* band 0 */
    src[40 * 64 + 50] = 1;       /* band 2 */
    TCHECK_EQ(csvnc_fb_blit(&fb, 0, 0, 64, 48, src, 256, CSVNC_SRC_XRGB8888, &dirty), 2);
    TCHECK_EQ(dirty.n, 2);
    TCHECK_EQ(csvnc_region_area(&dirty), 2);
    /* a full-height column of change: three band boxes that merge
     * into one column */
    csvnc_region_clear(&dirty);
    for (i = 0; i < 48; i++)
        src[i * 64 + 20] = 2;
    TCHECK_EQ(csvnc_fb_blit(&fb, 0, 0, 64, 48, src, 256, CSVNC_SRC_XRGB8888, &dirty), 48);
    TCHECK_EQ(dirty.n, 1);
    TCHECK_EQ(dirty.r[0].x, 20);
    TCHECK_EQ(dirty.r[0].w, 1);
    TCHECK_EQ(dirty.r[0].h, 48);
    free(src);
    csvnc_fb_free(&fb);
}

TEST_MAIN(test_native, test_novnc_format, test_rgb565, test_bgr233,
          test_big_endian_32, test_rejected, test_fb_blit,
          test_fb_blit_bands)
