/* Dirty-rectangle coalescing: a bounded list that always covers every
 * rectangle added to it. */

#include "csvnc-private.h"

/* Give the whole frame once the dirty area passes this fraction. */
#define CSVNC_REGION_FULL_NUM 3
#define CSVNC_REGION_FULL_DEN 4

void csvnc_region_init(csvnc_region *rg, int fw, int fh)
{
    memset(rg, 0, sizeof(*rg));
    rg->fw = fw;
    rg->fh = fh;
}

void csvnc_region_clear(csvnc_region *rg)
{
    rg->n = 0;
}

int csvnc_region_empty(const csvnc_region *rg)
{
    return rg->n == 0;
}

static long rect_area(const csvnc_rect *r)
{
    return (long)r->w * r->h;
}

long csvnc_region_area(const csvnc_region *rg)
{
    long a = 0;
    int i;

    for (i = 0; i < rg->n; i++)
        a += rect_area(&rg->r[i]);
    return a;
}

static csvnc_rect rect_union(const csvnc_rect *a, const csvnc_rect *b)
{
    csvnc_rect u;
    int ax1 = a->x + a->w, ay1 = a->y + a->h;
    int bx1 = b->x + b->w, by1 = b->y + b->h;

    u.x = a->x < b->x ? a->x : b->x;
    u.y = a->y < b->y ? a->y : b->y;
    u.w = (ax1 > bx1 ? ax1 : bx1) - u.x;
    u.h = (ay1 > by1 ? ay1 : by1) - u.y;
    return u;
}

/* Extra pixels sent by replacing A and B with their union.  Negative
 * when they overlap so much that the union is cheaper than both. */
static long merge_waste(const csvnc_rect *a, const csvnc_rect *b)
{
    csvnc_rect u = rect_union(a, b);

    return rect_area(&u) - rect_area(a) - rect_area(b);
}

static void remove_at(csvnc_region *rg, int i)
{
    rg->n--;
    if (i < rg->n)
        memmove(&rg->r[i], &rg->r[i + 1],
                (size_t)(rg->n - i) * sizeof(rg->r[0]));
}

void csvnc_region_add_all(csvnc_region *rg)
{
    rg->n = 1;
    rg->r[0].x = 0;
    rg->r[0].y = 0;
    rg->r[0].w = rg->fw;
    rg->r[0].h = rg->fh;
}

static void add_clipped(csvnc_region *rg, csvnc_rect r)
{
    int i;

    for (;;) {
        /* merge with anything the union does not waste area on;
         * restart the scan after each merge since the bigger rectangle
         * may now absorb others */
        for (i = 0; i < rg->n; i++) {
            if (merge_waste(&rg->r[i], &r) <= 0) {
                r = rect_union(&rg->r[i], &r);
                remove_at(rg, i);
                break;
            }
        }
        if (i == rg->n)
            break;
    }
    if (rg->n < CSVNC_REGION_MAX) {
        rg->r[rg->n++] = r;
    } else {
        /* full: force the least wasteful merge, then re-add the union
         * since it may enable free merges with the rest */
        int best = 0;
        long best_waste = merge_waste(&rg->r[0], &r);

        for (i = 1; i < rg->n; i++) {
            long w = merge_waste(&rg->r[i], &r);

            if (w < best_waste) {
                best_waste = w;
                best = i;
            }
        }
        r = rect_union(&rg->r[best], &r);
        remove_at(rg, best);
        add_clipped(rg, r);
        return;
    }
    if (csvnc_region_area(rg) * CSVNC_REGION_FULL_DEN
        >= (long)rg->fw * rg->fh * CSVNC_REGION_FULL_NUM)
        csvnc_region_add_all(rg);
}

void csvnc_region_add(csvnc_region *rg, int x, int y, int w, int h)
{
    csvnc_rect r;
    long x1, y1;

    if (w <= 0 || h <= 0 || x >= rg->fw || y >= rg->fh)
        return;
    x1 = (long)x + w;
    y1 = (long)y + h;
    if (x1 <= 0 || y1 <= 0)
        return;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x1 > rg->fw)
        x1 = rg->fw;
    if (y1 > rg->fh)
        y1 = rg->fh;
    r.x = x;
    r.y = y;
    r.w = (int)(x1 - x);
    r.h = (int)(y1 - y);
    add_clipped(rg, r);
}

int csvnc_region_pop(csvnc_region *rg, int max_rows, csvnc_rect *out)
{
    csvnc_rect *top;
    int i, first = 0;

    if (rg->n == 0)
        return 0;
    /* take the topmost rectangle so a frame streams out top to bottom */
    for (i = 1; i < rg->n; i++)
        if (rg->r[i].y < rg->r[first].y)
            first = i;
    top = &rg->r[first];
    if (max_rows > 0 && top->h > max_rows) {
        *out = *top;
        out->h = max_rows;
        top->y += max_rows;
        top->h -= max_rows;
        return 1;
    }
    *out = *top;
    remove_at(rg, first);
    return 1;
}
