/* Dirty-region coalescing: coverage is never lost, the list stays
 * bounded, exact merges cost nothing, the full-frame fallback fires. */

#include "../csvnc-private.h"
#include "test-util.h"

#define FW 640
#define FH 480

/* a coverage bitmap tracks everything ever added */
static uint8_t expect_cov[FH][FW];

static void cov_clear(void)
{
    memset(expect_cov, 0, sizeof(expect_cov));
}

static void cov_add(int x, int y, int w, int h)
{
    int i, j;

    for (j = y; j < y + h; j++)
        for (i = x; i < x + w; i++)
            if (i >= 0 && j >= 0 && i < FW && j < FH)
                expect_cov[j][i] = 1;
}

static void add(csvnc_region *rg, int x, int y, int w, int h)
{
    csvnc_region_add(rg, x, y, w, h);
    cov_add(x, y, w, h);
}

static int region_sane(const csvnc_region *rg)
{
    int i;

    if (rg->n < 0 || rg->n > CSVNC_REGION_MAX)
        return 0;
    for (i = 0; i < rg->n; i++) {
        const csvnc_rect *r = &rg->r[i];

        if (r->w <= 0 || r->h <= 0 || r->x < 0 || r->y < 0
            || r->x + r->w > rg->fw || r->y + r->h > rg->fh)
            return 0;
    }
    return 1;
}

/* every pixel ever added is covered by some rectangle */
static int region_covers(const csvnc_region *rg)
{
    int i, x, y;
    static uint8_t have[FH][FW];

    memset(have, 0, sizeof(have));
    for (i = 0; i < rg->n; i++) {
        const csvnc_rect *r = &rg->r[i];

        for (y = r->y; y < r->y + r->h; y++)
            for (x = r->x; x < r->x + r->w; x++)
                have[y][x] = 1;
    }
    for (y = 0; y < FH; y++)
        for (x = 0; x < FW; x++)
            if (expect_cov[y][x] && !have[y][x])
                return 0;
    return 1;
}

static uint32_t rng_state = 777;

static uint32_t rng(void)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return rng_state >> 8;
}

static void test_basic(void)
{
    csvnc_region rg;
    csvnc_rect r;

    csvnc_region_init(&rg, FW, FH);
    TCHECK(csvnc_region_empty(&rg));
    TCHECK_EQ(csvnc_region_pop(&rg, 0, &r), 0);
    cov_clear();
    add(&rg, 10, 10, 20, 20);
    TCHECK(!csvnc_region_empty(&rg));
    TCHECK_EQ(rg.n, 1);
    TCHECK_EQ(csvnc_region_area(&rg), 400);
    /* contained: no change */
    add(&rg, 15, 15, 5, 5);
    TCHECK_EQ(rg.n, 1);
    TCHECK_EQ(csvnc_region_area(&rg), 400);
    /* containing: replaces */
    add(&rg, 0, 0, 100, 100);
    TCHECK_EQ(rg.n, 1);
    TCHECK_EQ(csvnc_region_area(&rg), 10000);
    /* disjoint: second rectangle */
    add(&rg, 200, 200, 10, 10);
    TCHECK_EQ(rg.n, 2);
    TCHECK(region_covers(&rg));
    TCHECK_EQ(csvnc_region_pop(&rg, 0, &r), 1);
    TCHECK_EQ(r.x, 0);
    TCHECK_EQ(r.y, 0);
    TCHECK_EQ(r.w, 100);
    TCHECK_EQ(r.h, 100);
    TCHECK_EQ(csvnc_region_pop(&rg, 0, &r), 1);
    TCHECK_EQ(r.x, 200);
    TCHECK(csvnc_region_empty(&rg));
}

static void test_clipping(void)
{
    csvnc_region rg;

    csvnc_region_init(&rg, FW, FH);
    csvnc_region_add(&rg, -10, -10, 20, 20);
    TCHECK_EQ(rg.n, 1);
    TCHECK_EQ(rg.r[0].x, 0);
    TCHECK_EQ(rg.r[0].y, 0);
    TCHECK_EQ(rg.r[0].w, 10);
    TCHECK_EQ(rg.r[0].h, 10);
    csvnc_region_add(&rg, 630, 470, 100, 100);
    TCHECK_EQ(rg.n, 2);
    TCHECK_EQ(rg.r[1].w, 10);
    TCHECK_EQ(rg.r[1].h, 10);
    /* entirely outside, empty, or absurd: ignored */
    csvnc_region_add(&rg, 640, 0, 10, 10);
    csvnc_region_add(&rg, 0, 480, 10, 10);
    csvnc_region_add(&rg, -50, 0, 10, 10);
    csvnc_region_add(&rg, 0, 0, 0, 10);
    csvnc_region_add(&rg, 0, 0, 10, -1);
    csvnc_region_add(&rg, 2000000000, 0, 2000000000, 2000000000);
    csvnc_region_add(&rg, -2000000000, -2000000000, 2000000000, 2000000000);
    TCHECK_EQ(rg.n, 2);
    TCHECK(region_sane(&rg));
    /* huge rectangle over the whole frame clips to the frame */
    csvnc_region_add(&rg, -2000000000, -2000000000, 2000000000, 2000000000);
    csvnc_region_add(&rg, -100, -100, 1000, 1000);
    TCHECK_EQ(rg.n, 1);
    TCHECK_EQ(csvnc_region_area(&rg), (long)FW * FH);
}

static void test_exact_merges(void)
{
    csvnc_region rg;

    csvnc_region_init(&rg, FW, FH);
    cov_clear();
    /* horizontally adjacent strips merge into one */
    add(&rg, 0, 0, 16, 16);
    add(&rg, 16, 0, 16, 16);
    add(&rg, 32, 0, 16, 16);
    TCHECK_EQ(rg.n, 1);
    TCHECK_EQ(csvnc_region_area(&rg), 48 * 16);
    /* vertically adjacent too */
    add(&rg, 0, 16, 48, 16);
    TCHECK_EQ(rg.n, 1);
    TCHECK_EQ(csvnc_region_area(&rg), 48 * 32);
    /* overlapping with a waste-free union */
    add(&rg, 0, 30, 48, 10);
    TCHECK_EQ(rg.n, 1);
    TCHECK_EQ(csvnc_region_area(&rg), 48 * 40);
    /* diagonal neighbour: not merged (union would waste) */
    add(&rg, 48, 40, 16, 16);
    TCHECK_EQ(rg.n, 2);
    TCHECK(region_covers(&rg));
}

static void test_bounded(void)
{
    csvnc_region rg;
    int i, round;

    /* many scattered small rectangles: never more than the cap, never
     * losing coverage, and never the whole frame (the area added is
     * tiny) */
    csvnc_region_init(&rg, FW, FH);
    cov_clear();
    for (i = 0; i < 200; i++) {
        add(&rg, (i * 37) % 600, (i * 53) % 440, 8, 8);
        TCHECK(rg.n <= CSVNC_REGION_MAX);
        TCHECK(region_sane(&rg));
    }
    TCHECK(region_covers(&rg));
    TCHECK(csvnc_region_area(&rg) < (long)FW * FH);

    /* random rectangles of random sizes */
    for (round = 0; round < 20; round++) {
        csvnc_region_init(&rg, FW, FH);
        cov_clear();
        for (i = 0; i < 100; i++) {
            add(&rg, (int)(rng() % 700) - 30, (int)(rng() % 540) - 30,
                (int)(rng() % 120) + 1, (int)(rng() % 120) + 1);
            TCHECK(rg.n <= CSVNC_REGION_MAX);
            TCHECK(region_sane(&rg));
        }
        TCHECK(region_covers(&rg));
    }
}

static void test_full_frame_fallback(void)
{
    csvnc_region rg;

    csvnc_region_init(&rg, FW, FH);
    /* two disjoint rectangles covering > 3/4 of the frame collapse to
     * the whole frame */
    csvnc_region_add(&rg, 0, 0, 640, 200);
    TCHECK_EQ(rg.n, 1);
    TCHECK_EQ(csvnc_region_area(&rg), 640L * 200);
    csvnc_region_add(&rg, 0, 250, 640, 200);
    TCHECK_EQ(rg.n, 1);
    TCHECK_EQ(rg.r[0].x, 0);
    TCHECK_EQ(rg.r[0].y, 0);
    TCHECK_EQ(rg.r[0].w, FW);
    TCHECK_EQ(rg.r[0].h, FH);
    /* add_all directly */
    csvnc_region_init(&rg, FW, FH);
    csvnc_region_add(&rg, 5, 5, 5, 5);
    csvnc_region_add_all(&rg);
    TCHECK_EQ(rg.n, 1);
    TCHECK_EQ(csvnc_region_area(&rg), (long)FW * FH);
    /* adding more into a full frame changes nothing */
    csvnc_region_add(&rg, 5, 5, 5, 5);
    TCHECK_EQ(rg.n, 1);
    TCHECK_EQ(csvnc_region_area(&rg), (long)FW * FH);
}

static void test_pop_bands(void)
{
    csvnc_region rg;
    csvnc_rect r;
    int rows = 0, pops = 0;

    csvnc_region_init(&rg, FW, FH);
    csvnc_region_add(&rg, 100, 300, 10, 10);   /* lower, added first */
    csvnc_region_add(&rg, 0, 0, 640, 100);     /* topmost, popped first */
    TCHECK_EQ(csvnc_region_pop(&rg, 32, &r), 1);
    TCHECK_EQ(r.y, 0);
    TCHECK_EQ(r.h, 32);
    TCHECK_EQ(r.w, 640);
    rows += r.h;
    pops++;
    while (rows < 100) {
        TCHECK_EQ(csvnc_region_pop(&rg, 32, &r), 1);
        TCHECK_EQ(r.x, 0);
        TCHECK(r.h <= 32);
        rows += r.h;
        pops++;
    }
    TCHECK_EQ(rows, 100);
    TCHECK_EQ(pops, 4);
    TCHECK_EQ(csvnc_region_pop(&rg, 32, &r), 1);
    TCHECK_EQ(r.y, 300);
    TCHECK_EQ(r.h, 10);
    TCHECK(csvnc_region_empty(&rg));
    /* max_rows 0 = whole rectangle */
    csvnc_region_add(&rg, 0, 0, 640, 480);
    TCHECK_EQ(csvnc_region_pop(&rg, 0, &r), 1);
    TCHECK_EQ(r.h, 480);
    TCHECK(csvnc_region_empty(&rg));
    csvnc_region_add(&rg, 1, 1, 1, 1);
    csvnc_region_clear(&rg);
    TCHECK(csvnc_region_empty(&rg));
}

TEST_MAIN(test_basic, test_clipping, test_exact_merges, test_bounded,
          test_full_frame_fallback, test_pop_bands)
