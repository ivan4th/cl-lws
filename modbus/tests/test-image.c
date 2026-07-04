/* Slave register image tests: block registration, overlap rejection,
 * range reads/writes, writable/read-only/safe validation, the run-based
 * write-apply callback, plus csmb_request_values expansion.  Pure C, no
 * libwebsockets. */

#include "../csmb-private.h"
#include "test-util.h"

/* ---- apply-callback capture ---- */

#define MAX_RUNS 8
struct capture {
    int nruns;
    struct { uint16_t start, count, values[64]; } runs[MAX_RUNS];
};

static void capture_cb(void *ctx, uint16_t start, uint16_t count,
                       const uint16_t *values)
{
    struct capture *c = ctx;
    int i;

    if (c->nruns >= MAX_RUNS)
        return;
    c->runs[c->nruns].start = start;
    c->runs[c->nruns].count = count;
    for (i = 0; i < count && i < 64; i++)
        c->runs[c->nruns].values[i] = values[i];
    c->nruns++;
}

/* ---- registration & overlap ---- */

static void test_register_overlap(void)
{
    csmb_image img;

    csmb_image_init(&img);

    TCHECK_EQ(csmb_image_register(&img, 1, CSMB_HOLDING, 10, 10, 1, 0), CSMB_OK);
    /* identical range overlaps */
    TCHECK_EQ(csmb_image_register(&img, 1, CSMB_HOLDING, 10, 10, 1, 0),
              CSMB_EOVERLAP);
    /* straddling the front edge */
    TCHECK_EQ(csmb_image_register(&img, 1, CSMB_HOLDING, 5, 10, 1, 0),
              CSMB_EOVERLAP);
    /* straddling the back edge */
    TCHECK_EQ(csmb_image_register(&img, 1, CSMB_HOLDING, 15, 10, 1, 0),
              CSMB_EOVERLAP);
    /* abutting below (5..9) and above (20..24) is fine */
    TCHECK_EQ(csmb_image_register(&img, 1, CSMB_HOLDING, 5, 5, 1, 0), CSMB_OK);
    TCHECK_EQ(csmb_image_register(&img, 1, CSMB_HOLDING, 20, 5, 1, 0), CSMB_OK);
    /* a different reg type does not collide */
    TCHECK_EQ(csmb_image_register(&img, 1, CSMB_COIL, 10, 10, 1, 0), CSMB_OK);
    /* a different unit does not collide */
    TCHECK_EQ(csmb_image_register(&img, 2, CSMB_HOLDING, 10, 10, 1, 0), CSMB_OK);

    /* bad args */
    TCHECK_EQ(csmb_image_register(&img, 1, CSMB_HOLDING, 0, 0, 1, 0), CSMB_ERANGE);
    TCHECK_EQ(csmb_image_register(&img, 1, CSMB_HOLDING, 65530, 10, 1, 0),
              CSMB_ERANGE);   /* wraps past 65536 */
    TCHECK_EQ(csmb_image_register(&img, 1, 99, 0, 1, 1, 0), CSMB_EBADTYPE);

    TCHECK(csmb_image_unit_has_ranges(&img, 1));
    TCHECK(!csmb_image_unit_has_ranges(&img, 3));

    csmb_image_free(&img);
}

/* ---- reads ---- */

static void test_read_range(void)
{
    csmb_image img;
    uint16_t vals[8] = { 5, 6, 7 };
    uint16_t out[8];

    csmb_image_init(&img);
    TCHECK_EQ(csmb_image_register(&img, 1, CSMB_HOLDING, 100, 5, 0, 0), CSMB_OK);
    TCHECK_EQ(csmb_image_set_values(&img, 1, CSMB_HOLDING, 101, 3, vals), CSMB_OK);

    /* in range */
    TCHECK_EQ(csmb_image_read_range(&img, 1, CSMB_HOLDING, 100, 5, out),
              CSMB_EXC_NONE);
    TCHECK_EQ(out[0], 0);
    TCHECK_EQ(out[1], 5);
    TCHECK_EQ(out[2], 6);
    TCHECK_EQ(out[3], 7);
    TCHECK_EQ(out[4], 0);

    /* off the end -> illegal address */
    TCHECK_EQ(csmb_image_read_range(&img, 1, CSMB_HOLDING, 103, 5, out),
              CSMB_EXC_ILLEGAL_ADDRESS);
    /* unknown unit */
    TCHECK_EQ(csmb_image_read_range(&img, 9, CSMB_HOLDING, 100, 1, out),
              CSMB_EXC_ILLEGAL_ADDRESS);

    /* set_values that leaves a single block -> ERANGE */
    TCHECK_EQ(csmb_image_set_values(&img, 1, CSMB_HOLDING, 103, 5, vals),
              CSMB_ERANGE);

    csmb_image_free(&img);
}

/* ---- writes: writable, read-only, safe, mixed ---- */

static void test_write_apply(void)
{
    csmb_image img;
    struct capture cap;
    uint16_t wv[16];
    uint16_t out[16];
    int i;

    csmb_image_init(&img);
    TCHECK_EQ(csmb_image_register(&img, 1, CSMB_HOLDING, 10, 10, 1, 0), CSMB_OK);
    TCHECK_EQ(csmb_image_register(&img, 1, CSMB_HOLDING, 100, 5, 0, 0), CSMB_OK);
    TCHECK_EQ(csmb_image_register(&img, 1, CSMB_HOLDING, 200, 10, 0, 1), CSMB_OK);

    /* plain writable write: one run, values stored, readable back */
    for (i = 0; i < 4; i++)
        wv[i] = (uint16_t)(0x1000 + i);
    memset(&cap, 0, sizeof cap);
    TCHECK_EQ(csmb_image_write_range(&img, 1, CSMB_HOLDING, 12, 4, wv,
                                     capture_cb, &cap), CSMB_EXC_NONE);
    TCHECK_EQ(cap.nruns, 1);
    TCHECK_EQ(cap.runs[0].start, 12);
    TCHECK_EQ(cap.runs[0].count, 4);
    TCHECK_EQ(cap.runs[0].values[0], 0x1000);
    TCHECK_EQ(cap.runs[0].values[3], 0x1003);
    TCHECK_EQ(csmb_image_read_range(&img, 1, CSMB_HOLDING, 12, 4, out),
              CSMB_EXC_NONE);
    TCHECK_EQ(out[0], 0x1000);
    TCHECK_EQ(out[3], 0x1003);

    /* write into read-only block: illegal address, nothing applied, no run */
    memset(&cap, 0, sizeof cap);
    TCHECK_EQ(csmb_image_write_range(&img, 1, CSMB_HOLDING, 100, 3, wv,
                                     capture_cb, &cap),
              CSMB_EXC_ILLEGAL_ADDRESS);
    TCHECK_EQ(cap.nruns, 0);

    /* write into safe block: accepted, discarded (reads 0), no run/event */
    for (i = 0; i < 3; i++)
        wv[i] = (uint16_t)(0x2000 + i);
    memset(&cap, 0, sizeof cap);
    TCHECK_EQ(csmb_image_write_range(&img, 1, CSMB_HOLDING, 200, 3, wv,
                                     capture_cb, &cap), CSMB_EXC_NONE);
    TCHECK_EQ(cap.nruns, 0);
    TCHECK_EQ(csmb_image_read_range(&img, 1, CSMB_HOLDING, 200, 3, out),
              CSMB_EXC_NONE);
    TCHECK_EQ(out[0], 0);
    TCHECK_EQ(out[2], 0);

    /* write partly writable / partly unregistered: nothing applied */
    memset(&cap, 0, sizeof cap);
    TCHECK_EQ(csmb_image_write_range(&img, 1, CSMB_HOLDING, 18, 4, wv,
                                     capture_cb, &cap),
              CSMB_EXC_ILLEGAL_ADDRESS);   /* 20..21 unregistered */
    TCHECK_EQ(cap.nruns, 0);

    csmb_image_free(&img);
}

/* two adjacent writable blocks coalesce into one applied run */
static void test_write_coalesce(void)
{
    csmb_image img;
    struct capture cap;
    uint16_t wv[8];
    int i;

    csmb_image_init(&img);
    TCHECK_EQ(csmb_image_register(&img, 1, CSMB_HOLDING, 10, 5, 1, 0), CSMB_OK);
    TCHECK_EQ(csmb_image_register(&img, 1, CSMB_HOLDING, 15, 5, 1, 0), CSMB_OK);

    for (i = 0; i < 8; i++)
        wv[i] = (uint16_t)(i + 1);
    memset(&cap, 0, sizeof cap);
    TCHECK_EQ(csmb_image_write_range(&img, 1, CSMB_HOLDING, 12, 6, wv,
                                     capture_cb, &cap), CSMB_EXC_NONE);
    TCHECK_EQ(cap.nruns, 1);
    TCHECK_EQ(cap.runs[0].start, 12);
    TCHECK_EQ(cap.runs[0].count, 6);

    csmb_image_free(&img);
}

/* a writable stretch split by a safe stretch yields two runs */
static void test_write_split_runs(void)
{
    csmb_image img;
    struct capture cap;
    uint16_t wv[8];
    int i;

    csmb_image_init(&img);
    TCHECK_EQ(csmb_image_register(&img, 1, CSMB_HOLDING, 10, 2, 1, 0), CSMB_OK);
    TCHECK_EQ(csmb_image_register(&img, 1, CSMB_HOLDING, 12, 2, 0, 1), CSMB_OK);
    TCHECK_EQ(csmb_image_register(&img, 1, CSMB_HOLDING, 14, 2, 1, 0), CSMB_OK);

    for (i = 0; i < 6; i++)
        wv[i] = (uint16_t)(0x10 + i);
    memset(&cap, 0, sizeof cap);
    TCHECK_EQ(csmb_image_write_range(&img, 1, CSMB_HOLDING, 10, 6, wv,
                                     capture_cb, &cap), CSMB_EXC_NONE);
    TCHECK_EQ(cap.nruns, 2);
    TCHECK_EQ(cap.runs[0].start, 10);
    TCHECK_EQ(cap.runs[0].count, 2);
    TCHECK_EQ(cap.runs[0].values[0], 0x10);
    TCHECK_EQ(cap.runs[1].start, 14);
    TCHECK_EQ(cap.runs[1].count, 2);
    TCHECK_EQ(cap.runs[1].values[0], 0x14);   /* value slice offset by 4 */

    csmb_image_free(&img);
}

/* ---- coil storage (one uint16 per bit position) ---- */

static void test_coils(void)
{
    csmb_image img;
    struct capture cap;
    uint16_t wv[16];
    uint16_t out[16];
    int i;

    csmb_image_init(&img);
    TCHECK_EQ(csmb_image_register(&img, 1, CSMB_COIL, 0, 16, 1, 0), CSMB_OK);
    for (i = 0; i < 16; i++)
        wv[i] = (uint16_t)(i % 2);
    memset(&cap, 0, sizeof cap);
    TCHECK_EQ(csmb_image_write_range(&img, 1, CSMB_COIL, 0, 16, wv,
                                     capture_cb, &cap), CSMB_EXC_NONE);
    TCHECK_EQ(cap.nruns, 1);
    TCHECK_EQ(csmb_image_read_range(&img, 1, CSMB_COIL, 0, 16, out),
              CSMB_EXC_NONE);
    for (i = 0; i < 16; i++)
        TCHECK_EQ(out[i], (uint16_t)(i % 2));

    csmb_image_free(&img);
}

/* ---- csmb_request_values ---- */

static void test_request_values(void)
{
    uint8_t pdu[300];
    csmb_request req;
    uint16_t vals[32];
    uint16_t coils[20], regs[8];
    int n, i;

    /* FC5 coil on -> 1 */
    n = csmb_build_write_single(pdu, CSMB_COIL, 3, 1);
    TCHECK_EQ(csmb_decode_request(pdu, (size_t)n, &req), CSMB_OK);
    TCHECK_EQ(csmb_request_values(&req, vals), CSMB_OK);
    TCHECK_EQ(vals[0], 1);

    /* FC6 register value */
    n = csmb_build_write_single(pdu, CSMB_HOLDING, 3, 0xBEEF);
    TCHECK_EQ(csmb_decode_request(pdu, (size_t)n, &req), CSMB_OK);
    TCHECK_EQ(csmb_request_values(&req, vals), CSMB_OK);
    TCHECK_EQ(vals[0], 0xBEEF);

    /* FC15 bit unpack */
    for (i = 0; i < 20; i++)
        coils[i] = (uint16_t)(i % 3 == 0);
    n = csmb_build_write_multi(pdu, CSMB_COIL, 0, 20, coils);
    TCHECK_EQ(csmb_decode_request(pdu, (size_t)n, &req), CSMB_OK);
    TCHECK_EQ(csmb_request_values(&req, vals), CSMB_OK);
    for (i = 0; i < 20; i++)
        TCHECK_EQ(vals[i], (uint16_t)(i % 3 == 0));

    /* FC16 BE words */
    for (i = 0; i < 8; i++)
        regs[i] = (uint16_t)(0x1000 + i);
    n = csmb_build_write_multi(pdu, CSMB_HOLDING, 0, 8, regs);
    TCHECK_EQ(csmb_decode_request(pdu, (size_t)n, &req), CSMB_OK);
    TCHECK_EQ(csmb_request_values(&req, vals), CSMB_OK);
    for (i = 0; i < 8; i++)
        TCHECK_EQ(vals[i], (uint16_t)(0x1000 + i));

    /* a read fc is not a write */
    n = csmb_build_read(pdu, CSMB_HOLDING, 0, 4);
    TCHECK_EQ(csmb_decode_request(pdu, (size_t)n, &req), CSMB_OK);
    TCHECK(csmb_request_values(&req, vals) < 0);
}

TEST_MAIN(test_register_overlap, test_read_range,
          test_write_apply, test_write_coalesce, test_write_split_runs,
          test_coils, test_request_values)
