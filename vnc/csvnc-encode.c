/* Framebuffer encoders: Raw and Hextile, plus the FramebufferUpdate
 * message/rectangle headers.  ZRLE (zlib) plugs into csvnc_encode_rect
 * and csvnc_encoder later; the seam is the encoder struct + dispatch. */

#include "csvnc-private.h"

void csvnc_put_update_header(uint8_t *out, unsigned nrects)
{
    out[0] = CSVNC_SMSG_FB_UPDATE;
    out[1] = 0;
    csvnc_put_u16(out + 2, nrects);
}

void csvnc_put_rect_header(uint8_t *out, int x, int y, int w, int h,
                           int32_t enc)
{
    csvnc_put_u16(out, (unsigned)x);
    csvnc_put_u16(out + 2, (unsigned)y);
    csvnc_put_u16(out + 4, (unsigned)w);
    csvnc_put_u16(out + 6, (unsigned)h);
    csvnc_put_u32(out + 8, (uint32_t)enc);
}

void csvnc_encoder_init(csvnc_encoder *e)
{
    memset(e, 0, sizeof(*e));
    e->enc = CSVNC_ENC_RAW;
}

void csvnc_encoder_free(csvnc_encoder *e)
{
    /* ZRLE stream teardown goes here */
    e->zrle = NULL;
}

void csvnc_encoder_select(csvnc_encoder *e, int32_t enc)
{
    switch (enc) {
    case CSVNC_ENC_HEXTILE:
        e->enc = enc;
        break;
    default:
        e->enc = CSVNC_ENC_RAW;
        break;
    }
}

/* ---- Raw ---- */

size_t csvnc_encode_raw(const csvnc_pixfmt *f, const csvnc_fb *fb,
                        int x, int y, int w, int h,
                        uint8_t *out, size_t cap)
{
    size_t need = (size_t)w * (size_t)h * f->bytespp;
    uint8_t *p = out;
    int row;

    if (need > cap)
        return 0;
    for (row = 0; row < h; row++)
        p = csvnc_pix_put_row(f, p, fb->px + (size_t)(y + row) * fb->width + x, w);
    return need;
}

/* ---- Hextile ---- */

#define HEX_RAW        1
#define HEX_BG         2
#define HEX_FG         4
#define HEX_SUBRECTS   8
#define HEX_COLOURED   16

/* Beyond this many distinct colours a tile is sent raw without
 * bothering to decompose it. */
#define HEX_MAX_COLOURS 32
#define HEX_TILE_PIXELS (CSVNC_HEXTILE_TILE * CSVNC_HEXTILE_TILE)

typedef struct hex_state {
    uint32_t bg, fg;       /* colours known to the decoder (shadow values) */
    int valid_bg, valid_fg;
} hex_state;

typedef struct hex_subrect {
    uint8_t x, y, w, h;
    uint32_t colour;
} hex_subrect;

size_t csvnc_hextile_max_size(const csvnc_pixfmt *f, int w, int h)
{
    size_t tiles_x = ((size_t)w + CSVNC_HEXTILE_TILE - 1) / CSVNC_HEXTILE_TILE;
    size_t tiles_y = ((size_t)h + CSVNC_HEXTILE_TILE - 1) / CSVNC_HEXTILE_TILE;

    /* a tile is never coded bigger than raw + its subencoding byte */
    return (size_t)w * (size_t)h * f->bytespp + tiles_x * tiles_y;
}

/* Greedy decomposition of the non-background pixels of a tile into
 * rectangles: for each uncovered pixel take the longest run of its
 * colour to the right, then grow that run downwards while whole rows
 * match.  Returns the count or -1 when more than 255 would be needed. */
static int hex_subrects(const uint32_t *t, int tw, int th, uint32_t bg,
                        hex_subrect *subs)
{
    uint8_t done[HEX_TILE_PIXELS];
    int n = 0, x, y;

    memset(done, 0, (size_t)tw * th);
    for (y = 0; y < th; y++) {
        for (x = 0; x < tw; x++) {
            uint32_t c;
            int w, h, i, ok;

            if (done[y * tw + x])
                continue;
            c = t[y * tw + x];
            if (c == bg)
                continue;
            w = 1;
            while (x + w < tw && !done[y * tw + x + w] && t[y * tw + x + w] == c)
                w++;
            h = 1;
            ok = 1;
            while (ok && y + h < th) {
                for (i = 0; i < w; i++) {
                    if (done[(y + h) * tw + x + i]
                        || t[(y + h) * tw + x + i] != c) {
                        ok = 0;
                        break;
                    }
                }
                if (ok)
                    h++;
            }
            for (i = 0; i < h; i++)
                memset(done + (y + i) * tw + x, 1, (size_t)w);
            if (n == 255)
                return -1;
            subs[n].x = (uint8_t)x;
            subs[n].y = (uint8_t)y;
            subs[n].w = (uint8_t)w;
            subs[n].h = (uint8_t)h;
            subs[n].colour = c;
            n++;
        }
    }
    return n;
}

static size_t hex_raw_tile(const csvnc_pixfmt *f, const uint32_t *t,
                           int tw, int th, hex_state *st,
                           uint8_t *out, size_t cap)
{
    size_t need = 1 + (size_t)tw * th * f->bytespp;
    uint8_t *p = out + 1;
    int y;

    if (need > cap)
        return 0;
    out[0] = HEX_RAW;
    for (y = 0; y < th; y++)
        p = csvnc_pix_put_row(f, p, t + y * tw, tw);
    /* decoders differ on whether raw tiles keep the previous colours
     * (noVNC even ignores a blank tile right after a raw one), so make
     * the next tile restate them */
    st->valid_bg = st->valid_fg = 0;
    return need;
}

static size_t hex_tile(const csvnc_pixfmt *f, const uint32_t *t,
                       int tw, int th, hex_state *st,
                       uint8_t *out, size_t cap)
{
    uint32_t colours[HEX_MAX_COLOURS];
    int counts[HEX_MAX_COLOURS];
    hex_subrect subs[255];
    int ncol = 0, npix = tw * th, i, j, nsub = 0, best;
    uint32_t bg, fg = 0;
    size_t raw_cost = 1 + (size_t)npix * f->bytespp, cost;
    unsigned sub = 0;
    uint8_t *p;

    for (i = 0; i < npix; i++) {
        uint32_t c = t[i];

        for (j = 0; j < ncol; j++)
            if (colours[j] == c)
                break;
        if (j == ncol) {
            if (ncol == HEX_MAX_COLOURS)
                return hex_raw_tile(f, t, tw, th, st, out, cap);
            colours[ncol] = c;
            counts[ncol] = 0;
            ncol++;
        }
        counts[j]++;
    }
    best = 0;
    for (j = 1; j < ncol; j++)
        if (counts[j] > counts[best])
            best = j;
    bg = colours[best];

    cost = 1;
    if (!st->valid_bg || st->bg != bg) {
        sub |= HEX_BG;
        cost += f->bytespp;
    }
    if (ncol > 1) {
        nsub = hex_subrects(t, tw, th, bg, subs);
        if (nsub < 0)
            return hex_raw_tile(f, t, tw, th, st, out, cap);
        sub |= HEX_SUBRECTS;
        cost += 1;
        if (ncol == 2) {
            fg = colours[best == 0 ? 1 : 0];
            if (!st->valid_fg || st->fg != fg) {
                sub |= HEX_FG;
                cost += f->bytespp;
            }
            cost += (size_t)nsub * 2;
        } else {
            sub |= HEX_COLOURED;
            cost += (size_t)nsub * (2 + f->bytespp);
        }
    }
    if (cost >= raw_cost)
        return hex_raw_tile(f, t, tw, th, st, out, cap);
    if (cost > cap)
        return 0;

    p = out;
    *p++ = (uint8_t)sub;
    if (sub & HEX_BG) {
        csvnc_pix_put(f, p, csvnc_pix_translate(f, bg));
        p += f->bytespp;
        st->bg = bg;
        st->valid_bg = 1;
    }
    if (sub & HEX_FG) {
        csvnc_pix_put(f, p, csvnc_pix_translate(f, fg));
        p += f->bytespp;
        st->fg = fg;
        st->valid_fg = 1;
    }
    if (sub & HEX_SUBRECTS) {
        *p++ = (uint8_t)nsub;
        for (i = 0; i < nsub; i++) {
            if (sub & HEX_COLOURED) {
                csvnc_pix_put(f, p, csvnc_pix_translate(f, subs[i].colour));
                p += f->bytespp;
            }
            *p++ = (uint8_t)((subs[i].x << 4) | subs[i].y);
            *p++ = (uint8_t)(((subs[i].w - 1) << 4) | (subs[i].h - 1));
        }
    }
    return (size_t)(p - out);
}

size_t csvnc_encode_hextile(const csvnc_pixfmt *f, const csvnc_fb *fb,
                            int x, int y, int w, int h,
                            uint8_t *out, size_t cap)
{
    hex_state st;
    uint32_t tile[HEX_TILE_PIXELS];
    size_t used = 0;
    int ty, tx;

    memset(&st, 0, sizeof(st));
    for (ty = 0; ty < h; ty += CSVNC_HEXTILE_TILE) {
        int th = (h - ty < CSVNC_HEXTILE_TILE) ? h - ty : CSVNC_HEXTILE_TILE;

        for (tx = 0; tx < w; tx += CSVNC_HEXTILE_TILE) {
            int tw = (w - tx < CSVNC_HEXTILE_TILE) ? w - tx : CSVNC_HEXTILE_TILE;
            size_t n;
            int r;

            for (r = 0; r < th; r++)
                memcpy(tile + r * tw,
                       fb->px + (size_t)(y + ty + r) * fb->width + x + tx,
                       (size_t)tw * 4);
            n = hex_tile(f, tile, tw, th, &st, out + used, cap - used);
            if (!n)
                return 0;
            used += n;
        }
    }
    return used;
}

/* ---- dispatch ---- */

size_t csvnc_encode_max_size(const csvnc_encoder *e, const csvnc_pixfmt *f,
                             int w, int h)
{
    size_t payload;

    switch (e->enc) {
    case CSVNC_ENC_HEXTILE:
        payload = csvnc_hextile_max_size(f, w, h);
        break;
    default:
        payload = (size_t)w * (size_t)h * f->bytespp;
        break;
    }
    return CSVNC_RECT_HDR_SIZE + payload;
}

size_t csvnc_encode_rect(csvnc_encoder *e, const csvnc_pixfmt *f,
                         const csvnc_fb *fb, int x, int y, int w, int h,
                         uint8_t *out, size_t cap)
{
    size_t n;

    if (w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > fb->width
        || y + h > fb->height || cap < CSVNC_RECT_HDR_SIZE)
        return 0;
    switch (e->enc) {
    case CSVNC_ENC_HEXTILE:
        n = csvnc_encode_hextile(f, fb, x, y, w, h,
                                 out + CSVNC_RECT_HDR_SIZE,
                                 cap - CSVNC_RECT_HDR_SIZE);
        break;
    default:
        n = csvnc_encode_raw(f, fb, x, y, w, h, out + CSVNC_RECT_HDR_SIZE,
                             cap - CSVNC_RECT_HDR_SIZE);
        break;
    }
    if (!n)
        return 0;
    csvnc_put_rect_header(out, x, y, w, h, e->enc);
    return CSVNC_RECT_HDR_SIZE + n;
}
