/* Client pixel formats: parsing/validation, translation tables from the
 * XRGB8888 shadow, serialisation in the client's byte order; plus the
 * shadow framebuffer itself and the blit into it. */

#include "csvnc-private.h"

static int host_is_big_endian(void)
{
    const uint16_t probe = 0x0100;
    return *(const uint8_t *)&probe == 0x01;
}

/* Number of bits needed to hold MAX (max >= 1). */
static unsigned bits_for(unsigned max)
{
    unsigned n = 0;

    while (max) {
        n++;
        max >>= 1;
    }
    return n;
}

static void compile_tables(csvnc_pixfmt *f)
{
    unsigned i;

    for (i = 0; i < 256; i++) {
        f->rtab[i] = ((i * f->rmax + 127) / 255) << f->rshift;
        f->gtab[i] = ((i * f->gmax + 127) / 255) << f->gshift;
        f->btab[i] = ((i * f->bmax + 127) / 255) << f->bshift;
    }
    f->bytespp = (uint8_t)(f->bpp / 8);
    f->host_order = (f->big_endian != 0) == host_is_big_endian();
}

void csvnc_pixfmt_native(csvnc_pixfmt *f)
{
    memset(f, 0, sizeof(*f));
    f->bpp = 32;
    f->depth = 24;
    f->big_endian = 0;
    f->true_colour = 1;
    f->rmax = f->gmax = f->bmax = 255;
    f->rshift = 16;
    f->gshift = 8;
    f->bshift = 0;
    compile_tables(f);
}

static int channel_ok(unsigned max, unsigned shift, unsigned bpp)
{
    return max >= 1 && shift < bpp && shift + bits_for(max) <= bpp;
}

int csvnc_pixfmt_parse(csvnc_pixfmt *f, const uint8_t wire[16])
{
    csvnc_pixfmt t;

    memset(&t, 0, sizeof(t));
    t.bpp = wire[0];
    t.depth = wire[1];
    t.big_endian = wire[2] != 0;
    t.true_colour = wire[3];
    t.rmax = (uint16_t)csvnc_get_u16(wire + 4);
    t.gmax = (uint16_t)csvnc_get_u16(wire + 6);
    t.bmax = (uint16_t)csvnc_get_u16(wire + 8);
    t.rshift = wire[10];
    t.gshift = wire[11];
    t.bshift = wire[12];

    if (t.bpp != 8 && t.bpp != 16 && t.bpp != 32)
        return -1;
    if (t.depth < 1 || t.depth > t.bpp)
        return -1;
    if (!t.true_colour)
        return -1;                     /* colour maps unsupported */
    if (!channel_ok(t.rmax, t.rshift, t.bpp)
        || !channel_ok(t.gmax, t.gshift, t.bpp)
        || !channel_ok(t.bmax, t.bshift, t.bpp))
        return -1;
    t.true_colour = 1;
    compile_tables(&t);
    *f = t;
    return 0;
}

void csvnc_pixfmt_write(const csvnc_pixfmt *f, uint8_t wire[16])
{
    memset(wire, 0, 16);
    wire[0] = f->bpp;
    wire[1] = f->depth;
    wire[2] = f->big_endian;
    wire[3] = f->true_colour;
    csvnc_put_u16(wire + 4, f->rmax);
    csvnc_put_u16(wire + 6, f->gmax);
    csvnc_put_u16(wire + 8, f->bmax);
    wire[10] = f->rshift;
    wire[11] = f->gshift;
    wire[12] = f->bshift;
}

void csvnc_pix_put(const csvnc_pixfmt *f, uint8_t *dst, uint32_t v)
{
    switch (f->bytespp) {
    case 1:
        dst[0] = (uint8_t)v;
        break;
    case 2:
        if (f->big_endian) {
            dst[0] = (uint8_t)(v >> 8);
            dst[1] = (uint8_t)v;
        } else {
            dst[0] = (uint8_t)v;
            dst[1] = (uint8_t)(v >> 8);
        }
        break;
    default:
        if (f->big_endian)
            csvnc_put_u32(dst, v);
        else {
            dst[0] = (uint8_t)v;
            dst[1] = (uint8_t)(v >> 8);
            dst[2] = (uint8_t)(v >> 16);
            dst[3] = (uint8_t)(v >> 24);
        }
        break;
    }
}

uint32_t csvnc_pix_get(const csvnc_pixfmt *f, const uint8_t *src)
{
    switch (f->bytespp) {
    case 1:
        return src[0];
    case 2:
        return f->big_endian ? csvnc_get_u16(src)
            : ((uint32_t)src[1] << 8) | src[0];
    default:
        return f->big_endian ? csvnc_get_u32(src)
            : ((uint32_t)src[3] << 24) | ((uint32_t)src[2] << 16)
              | ((uint32_t)src[1] << 8) | src[0];
    }
}

uint8_t *csvnc_pix_put_row(const csvnc_pixfmt *f, uint8_t *dst,
                           const uint32_t *src, int n)
{
    int i;

    if (f->bytespp == 4 && f->host_order) {
        /* the common case (noVNC, native): plain 32-bit stores */
        for (i = 0; i < n; i++) {
            uint32_t v = csvnc_pix_translate(f, src[i]);

            memcpy(dst, &v, 4);
            dst += 4;
        }
        return dst;
    }
    if (f->bytespp == 1) {
        for (i = 0; i < n; i++)
            *dst++ = (uint8_t)csvnc_pix_translate(f, src[i]);
        return dst;
    }
    for (i = 0; i < n; i++) {
        csvnc_pix_put(f, dst, csvnc_pix_translate(f, src[i]));
        dst += f->bytespp;
    }
    return dst;
}

/* ---- shadow framebuffer ---- */

int csvnc_fb_init(csvnc_fb *fb, int width, int height)
{
    memset(fb, 0, sizeof(*fb));
    if (width < 1 || height < 1 || width > CSVNC_MAX_DIM
        || height > CSVNC_MAX_DIM)
        return -1;
    fb->px = calloc((size_t)width * (size_t)height, sizeof(uint32_t));
    fb->row = calloc((size_t)width, sizeof(uint32_t));
    if (!fb->px || !fb->row) {
        csvnc_fb_free(fb);
        return -1;
    }
    fb->width = width;
    fb->height = height;
    return 0;
}

void csvnc_fb_free(csvnc_fb *fb)
{
    free(fb->px);
    free(fb->row);
    fb->px = fb->row = NULL;
    fb->width = fb->height = 0;
}

static inline uint32_t rgb565_to_xrgb(unsigned v)
{
    unsigned r = (v >> 11) & 0x1f, g = (v >> 5) & 0x3f, b = v & 0x1f;

    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);
    return (r << 16) | (g << 8) | b;
}

/* Convert one source row into ROW (canonical 0x00RRGGBB). */
static void convert_row(uint32_t *row, const uint8_t *src, int n, int fmt)
{
    int i;

    if (fmt == CSVNC_SRC_RGB565) {
        for (i = 0; i < n; i++) {
            uint16_t v;

            memcpy(&v, src + (size_t)i * 2, 2);
            row[i] = rgb565_to_xrgb(v);
        }
    } else {
        for (i = 0; i < n; i++) {
            uint32_t v;

            memcpy(&v, src + (size_t)i * 4, 4);
            row[i] = v & 0x00ffffffu;
        }
    }
}

int csvnc_fb_blit(csvnc_fb *fb, int x, int y, int w, int h,
                  const void *px, int stride_bytes, int src_format,
                  struct csvnc_region *dirty)
{
    int x0 = x, y0 = y, x1, y1, row, changed_rows = 0;
    const uint8_t *src = px;
    int band = -1, bx0 = 0, bx1 = 0, by0 = 0, by1 = 0;

    if (w <= 0 || h <= 0 || x0 >= fb->width || y0 >= fb->height
        || (long)x0 + w <= 0 || (long)y0 + h <= 0)
        return 0;
    x1 = ((long)x0 + w > fb->width) ? fb->width : x0 + w;
    y1 = ((long)y0 + h > fb->height) ? fb->height : y0 + h;
    if (x0 < 0) {
        src += (size_t)(-x0) * (src_format == CSVNC_SRC_RGB565 ? 2 : 4);
        x0 = 0;
    }
    if (y0 < 0) {
        src += (size_t)(-y0) * (size_t)stride_bytes;
        y0 = 0;
    }
    w = x1 - x0;

    for (row = y0; row < y1; row++, src += stride_bytes) {
        uint32_t *dst = fb->px + (size_t)row * fb->width + x0;
        int first, last;

        convert_row(fb->row, src, w, src_format);
        if (memcmp(dst, fb->row, (size_t)w * 4) == 0)
            continue;
        for (first = 0; dst[first] == fb->row[first]; first++)
            ;
        for (last = w - 1; dst[last] == fb->row[last]; last--)
            ;
        memcpy(dst + first, fb->row + first, (size_t)(last - first + 1) * 4);
        changed_rows++;
        if (!dirty)
            continue;
        if (row / CSVNC_HEXTILE_TILE != band) {
            if (band >= 0)
                csvnc_region_add(dirty, bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1);
            band = row / CSVNC_HEXTILE_TILE;
            bx0 = x0 + first;
            bx1 = x0 + last;
            by0 = by1 = row;
        } else {
            if (x0 + first < bx0)
                bx0 = x0 + first;
            if (x0 + last > bx1)
                bx1 = x0 + last;
            by1 = row;
        }
    }
    if (dirty && band >= 0)
        csvnc_region_add(dirty, bx0, by0, bx1 - bx0 + 1, by1 - by0 + 1);
    return changed_rows;
}
