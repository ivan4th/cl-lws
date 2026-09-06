/* ZRLE: 64x64 tiles, each as the cheapest of solid / raw / packed
 * palette / plain RLE / palette RLE (byte cost before zlib decides),
 * the whole rectangle's tile stream deflated through the client's
 * one zlib stream (the spec ties the stream to the connection, so it
 * lives in csvnc_encoder and survives encoding switches). */

#include <zlib.h>
#include "csvnc-private.h"

#define ZRLE_TILE 64
#define ZRLE_TILE_PIXELS (ZRLE_TILE * ZRLE_TILE)
#define ZRLE_MAX_PALETTE 127
#define ZRLE_COMPRESSION_LEVEL 3

typedef struct csvnc_zrle {
    z_stream strm;
    uint8_t *tmp;          /* the rectangle's uncompressed tile stream */
    size_t tmp_cap;
} csvnc_zrle;

/* ---- CPIXEL ---- */

typedef struct cpixel {
    int bytes;       /* bytes per CPIXEL on the wire */
    int offset;      /* which bytes of the 4-byte wire pixel (32 bpp) */
} cpixel;

static cpixel cpixel_of(const csvnc_pixfmt *f)
{
    cpixel c;

    c.bytes = f->bytespp;
    c.offset = 0;
    if (f->bpp == 32 && f->depth <= 24) {
        uint32_t max = csvnc_pix_translate(f, 0x00ffffff);
        int fits_ls3 = max < (1u << 24), fits_ms3 = (max & 0xff) == 0;

        if ((fits_ls3 && !f->big_endian) || (fits_ms3 && f->big_endian)) {
            c.bytes = 3;
            c.offset = 0;
        } else if ((fits_ls3 && f->big_endian) || (fits_ms3 && !f->big_endian)) {
            c.bytes = 3;
            c.offset = 1;
        }
    }
    return c;
}

static uint8_t *put_cpixel(const csvnc_pixfmt *f, const cpixel *c,
                           uint8_t *dst, uint32_t xrgb)
{
    uint8_t wire[4];

    csvnc_pix_put(f, wire, csvnc_pix_translate(f, xrgb));
    memcpy(dst, wire + c->offset, (size_t)c->bytes);
    return dst + c->bytes;
}

size_t csvnc_zrle_cpixel_bytes(const csvnc_pixfmt *f)
{
    return (size_t)cpixel_of(f).bytes;
}

/* ---- run lengths ---- */

/* bytes needed for a run of LEN pixels in RLE form (length - 1 in
 * base-255 continuation bytes) */
static size_t rle_len_bytes(int len)
{
    return (size_t)((len - 1) / 255) + 1;
}

static uint8_t *put_rle_len(uint8_t *p, int len)
{
    int n = len - 1;

    while (n >= 255) {
        *p++ = 255;
        n -= 255;
    }
    *p++ = (uint8_t)n;
    return p;
}

/* ---- one tile ---- */

typedef struct tile_stats {
    uint32_t palette[ZRLE_MAX_PALETTE + 1];
    int ncol;                      /* > ZRLE_MAX_PALETTE: too many */
    int nruns;
    size_t plain_rle_cost;         /* run bodies without the subencoding byte */
    size_t palette_rle_cost;
} tile_stats;

static int palette_index(const uint32_t *palette, int ncol, uint32_t c)
{
    int i;

    for (i = 0; i < ncol; i++)
        if (palette[i] == c)
            return i;
    return -1;
}

static void analyse(const uint32_t *t, int npix, int cp, tile_stats *st)
{
    int i, run = 1;
    uint32_t prev = t[0];

    st->ncol = 1;
    st->palette[0] = prev;
    st->nruns = 0;
    st->plain_rle_cost = st->palette_rle_cost = 0;
    for (i = 1; i <= npix; i++) {
        uint32_t c = i < npix ? t[i] : prev + 1;   /* sentinel ends the last run */

        if (i < npix && c == prev) {
            run++;
            continue;
        }
        /* a run of PREV of length RUN ends */
        st->nruns++;
        st->plain_rle_cost += (size_t)cp + rle_len_bytes(run);
        st->palette_rle_cost += run == 1 ? 1 : 1 + rle_len_bytes(run);
        if (i < npix) {
            if (st->ncol <= ZRLE_MAX_PALETTE
                && palette_index(st->palette, st->ncol, c) < 0) {
                if (st->ncol < ZRLE_MAX_PALETTE)
                    st->palette[st->ncol] = c;
                st->ncol++;
            }
            prev = c;
            run = 1;
        }
    }
}

static int packed_bits(int ncol)
{
    return ncol <= 2 ? 1 : ncol <= 4 ? 2 : 4;
}

/* Encode one TW x TH tile (pixels in T, row-major) at P; returns the
 * end pointer. */
static uint8_t *encode_tile(const csvnc_pixfmt *f, const cpixel *cp,
                            const uint32_t *t, int tw, int th, uint8_t *p)
{
    tile_stats st;
    int npix = tw * th, i, x, y;
    size_t cost_raw, cost_packed = (size_t)-1, cost_prle = (size_t)-1,
        cost_rle, best;
    enum { RAW, SOLID, PACKED, RLE, PRLE } choice;

    analyse(t, npix, cp->bytes, &st);
    if (st.ncol == 1) {
        *p++ = 1;
        return put_cpixel(f, cp, p, t[0]);
    }
    cost_raw = (size_t)npix * cp->bytes;
    cost_rle = st.plain_rle_cost;
    if (st.ncol <= 16)
        cost_packed = (size_t)st.ncol * cp->bytes
            + (size_t)th * (((size_t)tw * packed_bits(st.ncol) + 7) / 8);
    if (st.ncol <= ZRLE_MAX_PALETTE)
        cost_prle = (size_t)st.ncol * cp->bytes + st.palette_rle_cost;

    choice = RAW;
    best = cost_raw;
    if (cost_packed < best) {
        choice = PACKED;
        best = cost_packed;
    }
    if (cost_rle < best) {
        choice = RLE;
        best = cost_rle;
    }
    if (cost_prle < best) {
        choice = PRLE;
        best = cost_prle;
    }

    switch (choice) {
    case RAW:
        *p++ = 0;
        for (i = 0; i < npix; i++)
            p = put_cpixel(f, cp, p, t[i]);
        break;
    case PACKED: {
        int bits = packed_bits(st.ncol);

        *p++ = (uint8_t)st.ncol;
        for (i = 0; i < st.ncol; i++)
            p = put_cpixel(f, cp, p, st.palette[i]);
        for (y = 0; y < th; y++) {
            unsigned acc = 0;
            int nbits = 0;

            for (x = 0; x < tw; x++) {
                acc = (acc << bits)
                    | (unsigned)palette_index(st.palette, st.ncol, t[y * tw + x]);
                nbits += bits;
                if (nbits == 8) {
                    *p++ = (uint8_t)acc;
                    acc = 0;
                    nbits = 0;
                }
            }
            if (nbits)
                *p++ = (uint8_t)(acc << (8 - nbits));
        }
        break;
    }
    case RLE:
    case PRLE: {
        int run = 1;
        uint32_t prev = t[0];

        if (choice == RLE) {
            *p++ = 128;
        } else {
            *p++ = (uint8_t)(128 + st.ncol);
            for (i = 0; i < st.ncol; i++)
                p = put_cpixel(f, cp, p, st.palette[i]);
        }
        for (i = 1; i <= npix; i++) {
            if (i < npix && t[i] == prev) {
                run++;
                continue;
            }
            if (choice == RLE) {
                p = put_cpixel(f, cp, p, prev);
                p = put_rle_len(p, run);
            } else {
                int idx = palette_index(st.palette, st.ncol, prev);

                if (run == 1) {
                    *p++ = (uint8_t)idx;
                } else {
                    *p++ = (uint8_t)(idx | 128);
                    p = put_rle_len(p, run);
                }
            }
            if (i < npix) {
                prev = t[i];
                run = 1;
            }
        }
        break;
    }
    default:
        break;
    }
    return p;
}

/* ---- the rectangle ---- */

/* Bound on the uncompressed tile stream of a W x H rectangle: one
 * subencoding byte per tile plus raw pixels (no other choice is ever
 * bigger than raw). */
static size_t tile_stream_bound(const csvnc_pixfmt *f, int w, int h)
{
    size_t tx = ((size_t)w + ZRLE_TILE - 1) / ZRLE_TILE;
    size_t ty = ((size_t)h + ZRLE_TILE - 1) / ZRLE_TILE;

    return tx * ty + (size_t)w * (size_t)h * csvnc_zrle_cpixel_bytes(f);
}

/* zlib's generic bound for any deflate output of LEN input bytes
 * (stored blocks included), plus a sync-flush marker. */
static size_t deflate_bound(size_t len)
{
    return len + ((len + 7) >> 3) + ((len + 63) >> 6) + 5 + 16;
}

size_t csvnc_zrle_max_size(const csvnc_pixfmt *f, int w, int h)
{
    return 4 + deflate_bound(tile_stream_bound(f, w, h));
}

static csvnc_zrle *zrle_of(csvnc_encoder *e)
{
    csvnc_zrle *z = e->zrle;

    if (z)
        return z;
    z = calloc(1, sizeof(*z));
    if (!z)
        return NULL;
    if (deflateInit(&z->strm, ZRLE_COMPRESSION_LEVEL) != Z_OK) {
        free(z);
        return NULL;
    }
    e->zrle = z;
    return z;
}

void csvnc_zrle_free(csvnc_encoder *e)
{
    csvnc_zrle *z = e->zrle;

    if (!z)
        return;
    deflateEnd(&z->strm);
    free(z->tmp);
    free(z);
    e->zrle = NULL;
}

size_t csvnc_encode_zrle(csvnc_encoder *e, const csvnc_pixfmt *f,
                         const csvnc_fb *fb, int x, int y, int w, int h,
                         uint8_t *out, size_t cap)
{
    csvnc_zrle *z = zrle_of(e);
    cpixel cp = cpixel_of(f);
    uint32_t tile[ZRLE_TILE_PIXELS];
    size_t need = tile_stream_bound(f, w, h), produced;
    uint8_t *p;
    int ty, tx, r, rc;

    if (!z || cap < 4)
        return 0;
    if (z->tmp_cap < need) {
        uint8_t *tmp = realloc(z->tmp, need);

        if (!tmp)
            return 0;
        z->tmp = tmp;
        z->tmp_cap = need;
    }
    p = z->tmp;
    for (ty = 0; ty < h; ty += ZRLE_TILE) {
        int th = h - ty < ZRLE_TILE ? h - ty : ZRLE_TILE;

        for (tx = 0; tx < w; tx += ZRLE_TILE) {
            int tw = w - tx < ZRLE_TILE ? w - tx : ZRLE_TILE;

            for (r = 0; r < th; r++)
                memcpy(tile + r * tw,
                       fb->px + (size_t)(y + ty + r) * fb->width + x + tx,
                       (size_t)tw * 4);
            p = encode_tile(f, &cp, tile, tw, th, p);
        }
    }
    z->strm.next_in = z->tmp;
    z->strm.avail_in = (uInt)(p - z->tmp);
    z->strm.next_out = out + 4;
    z->strm.avail_out = (uInt)(cap - 4);
    rc = deflate(&z->strm, Z_SYNC_FLUSH);
    if (rc != Z_OK || z->strm.avail_in != 0) {
        /* the output did not fit: the stream is now out of step with
         * the client, which must be dropped */
        return 0;
    }
    produced = (size_t)(z->strm.next_out - (out + 4));
    csvnc_put_u32(out, (uint32_t)produced);
    return 4 + produced;
}
