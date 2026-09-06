/* Reference ZRLE decoder for the tests, written against the RFB
 * community spec (and noVNC's decoders/zrle.js behaviour): one
 * inflate stream per client, 64x64 tiles, subencodings 0 (raw),
 * 1 (solid), 2..16 (packed palette), 128 (plain RLE), 130..255
 * (palette RLE).  CPIXEL follows the spec's 3-byte rule. */

#ifndef CSVNC_ZRLE_REF_H
#define CSVNC_ZRLE_REF_H

#include <zlib.h>
#include "../csvnc-private.h"

typedef struct zrle_ref {
    z_stream strm;
    int inited;
    uint8_t *buf;
    size_t cap, len, pos;
    int error;
} zrle_ref;

static void zrle_ref_init(zrle_ref *z)
{
    memset(z, 0, sizeof(*z));
    inflateInit(&z->strm);
    z->inited = 1;
}

static void zrle_ref_free(zrle_ref *z)
{
    if (z->inited)
        inflateEnd(&z->strm);
    free(z->buf);
    memset(z, 0, sizeof(*z));
}

static int zrle_ref_cpixel(const csvnc_pixfmt *f, int *offset)
{
    *offset = 0;
    if (f->bpp == 32 && f->depth <= 24) {
        uint32_t max = csvnc_pix_translate(f, 0x00ffffff);
        int ls3 = max < (1u << 24), ms3 = (max & 0xff) == 0;

        if ((ls3 && !f->big_endian) || (ms3 && f->big_endian))
            return 3;
        if ((ls3 && f->big_endian) || (ms3 && !f->big_endian)) {
            *offset = 1;
            return 3;
        }
    }
    return f->bytespp;
}

static int zrle_ref_byte(zrle_ref *z)
{
    if (z->pos >= z->len) {
        z->error = 1;
        return 0;
    }
    return z->buf[z->pos++];
}

static uint32_t zrle_ref_pixel(zrle_ref *z, const csvnc_pixfmt *f, int cp, int off)
{
    uint8_t wire[4] = { 0, 0, 0, 0 };
    int i;

    for (i = 0; i < cp; i++)
        wire[off + i] = (uint8_t)zrle_ref_byte(z);
    return csvnc_pix_get(f, wire);
}

static int zrle_ref_runlen(zrle_ref *z)
{
    int len = 1, b;

    do {
        b = zrle_ref_byte(z);
        len += b;
    } while (b == 255 && !z->error);
    return len;
}

/* Decode the ZRLE payload IN (length-prefixed) of a W x H rectangle
 * into OUT (client pixel values).  Returns the payload bytes consumed
 * (4 + length) or -1. */
static long zrle_ref_decode(zrle_ref *z, const csvnc_pixfmt *f,
                            const uint8_t *in, size_t inlen,
                            int w, int h, uint32_t *out)
{
    uint32_t zlen;
    int cp, off, ty, tx, rc;

    if (inlen < 4)
        return -1;
    zlen = csvnc_get_u32(in);
    if (4 + (size_t)zlen > inlen)
        return -1;
    cp = zrle_ref_cpixel(f, &off);
    /* inflate everything of this rectangle */
    z->len = z->pos = 0;
    z->strm.next_in = (Bytef *)(in + 4);
    z->strm.avail_in = zlen;
    do {
        if (z->len == z->cap) {
            z->cap = z->cap ? z->cap * 2 : 65536;
            z->buf = realloc(z->buf, z->cap);
        }
        z->strm.next_out = z->buf + z->len;
        z->strm.avail_out = (uInt)(z->cap - z->len);
        rc = inflate(&z->strm, Z_SYNC_FLUSH);
        z->len = z->cap - z->strm.avail_out;
        if (rc != Z_OK && rc != Z_BUF_ERROR)
            return -1;
    } while (z->strm.avail_in > 0 || z->strm.avail_out == 0);
    z->error = 0;

    for (ty = 0; ty < h; ty += 64) {
        int th = h - ty < 64 ? h - ty : 64;

        for (tx = 0; tx < w; tx += 64) {
            int tw = w - tx < 64 ? w - tx : 64;
            int npix = tw * th, sub = zrle_ref_byte(z), i, x, y;
            uint32_t palette[128];

            if (sub == 0) {
                for (y = 0; y < th; y++)
                    for (x = 0; x < tw; x++)
                        out[(ty + y) * w + tx + x] = zrle_ref_pixel(z, f, cp, off);
            } else if (sub == 1) {
                uint32_t c = zrle_ref_pixel(z, f, cp, off);

                for (y = 0; y < th; y++)
                    for (x = 0; x < tw; x++)
                        out[(ty + y) * w + tx + x] = c;
            } else if (sub >= 2 && sub <= 16) {
                int bits = sub <= 2 ? 1 : sub <= 4 ? 2 : 4;

                for (i = 0; i < sub; i++)
                    palette[i] = zrle_ref_pixel(z, f, cp, off);
                for (y = 0; y < th; y++) {
                    int acc = 0, nbits = 0;

                    for (x = 0; x < tw; x++) {
                        int idx;

                        if (nbits == 0) {
                            acc = zrle_ref_byte(z);
                            nbits = 8;
                        }
                        nbits -= bits;
                        idx = (acc >> nbits) & ((1 << bits) - 1);
                        if (idx >= sub)
                            return -1;
                        out[(ty + y) * w + tx + x] = palette[idx];
                    }
                }
            } else if (sub == 128) {
                i = 0;
                while (i < npix && !z->error) {
                    uint32_t c = zrle_ref_pixel(z, f, cp, off);
                    int len = zrle_ref_runlen(z);

                    if (i + len > npix)
                        return -1;
                    while (len--) {
                        out[(ty + i / tw) * w + tx + i % tw] = c;
                        i++;
                    }
                }
            } else if (sub >= 130) {
                int ncol = sub - 128;

                for (i = 0; i < ncol; i++)
                    palette[i] = zrle_ref_pixel(z, f, cp, off);
                i = 0;
                while (i < npix && !z->error) {
                    int idx = zrle_ref_byte(z), len = 1;

                    if (idx & 128) {
                        idx &= 127;
                        len = zrle_ref_runlen(z);
                    }
                    if (idx >= ncol || i + len > npix)
                        return -1;
                    while (len--) {
                        out[(ty + i / tw) * w + tx + i % tw] = palette[idx];
                        i++;
                    }
                }
            } else {
                return -1;   /* 17..127, 129: unused */
            }
            if (z->error)
                return -1;
        }
    }
    if (z->pos != z->len)
        return -1;       /* trailing bytes in the tile stream */
    return 4 + (long)zlen;
}

#endif /* CSVNC_ZRLE_REF_H */
