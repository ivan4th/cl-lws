/* RFB handshake + client message parser: every message, fed whole,
 * byte by byte and in random chunks; bad lengths and junk. */

#include "../csvnc-private.h"
#include <stdarg.h>
#include "test-util.h"

/* ---- recording ops ---- */

#define LOG_MAX 4096

typedef struct rec {
    uint8_t out[4096];          /* everything the server sent */
    size_t outlen;
    char log[LOG_MAX];          /* textual event log */
    size_t loglen;
    const csvnc_rfb *conn;
} rec;

static void logf_(rec *r, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(r->log + r->loglen, LOG_MAX - r->loglen, fmt, ap);
    va_end(ap);
    if (n > 0 && (size_t)n < LOG_MAX - r->loglen)
        r->loglen += (size_t)n;
}

static void op_send(void *u, const uint8_t *d, size_t n)
{
    rec *r = u;

    if (r->outlen + n <= sizeof(r->out)) {
        memcpy(r->out + r->outlen, d, n);
        r->outlen += n;
    }
}

static void op_pixel_format(void *u)
{
    rec *r = u;
    const csvnc_pixfmt *f = &r->conn->fmt;

    logf_(r, "pf(%d,%d,%d,%d/%d/%d,%d/%d/%d);", f->bpp, f->depth, f->big_endian,
          f->rmax, f->gmax, f->bmax, f->rshift, f->gshift, f->bshift);
}

static void op_encodings(void *u)
{
    rec *r = u;

    logf_(r, "enc(%d,lr=%d);", (int)r->conn->enc, r->conn->lastrect);
}

static void op_update_request(void *u, int incr, int x, int y, int w, int h)
{
    logf_((rec *)u, "fbur(%d,%d,%d,%d,%d);", incr, x, y, w, h);
}

static void op_key(void *u, uint32_t ks, int down)
{
    logf_((rec *)u, "key(%x,%d);", (unsigned)ks, down);
}

static void op_pointer(void *u, int x, int y, unsigned b)
{
    logf_((rec *)u, "ptr(%d,%d,%u);", x, y, b);
}

static const csvnc_rfb_ops ops = {
    op_send, op_pixel_format, op_encodings, op_update_request, op_key, op_pointer
};

static void setup(csvnc_rfb *c, rec *r)
{
    memset(r, 0, sizeof(*r));
    r->conn = c;
    csvnc_rfb_init(c, &ops, r, 640, 480, "EACS console");
}

static int feed_all(csvnc_rfb *c, const uint8_t *d, size_t n)
{
    return csvnc_rfb_feed(c, d, n);
}

static int feed_bytewise(csvnc_rfb *c, const uint8_t *d, size_t n)
{
    size_t i;
    int rc = 0;

    for (i = 0; i < n && rc == 0; i++)
        rc = csvnc_rfb_feed(c, d + i, 1);
    return rc;
}

static uint32_t rng_state = 12345;

static uint32_t rng(void)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return rng_state >> 8;
}

static int feed_chunked(csvnc_rfb *c, const uint8_t *d, size_t n)
{
    size_t i = 0;
    int rc = 0;

    while (i < n && rc == 0) {
        size_t take = 1 + rng() % 7;

        if (take > n - i)
            take = n - i;
        rc = csvnc_rfb_feed(c, d + i, take);
        i += take;
    }
    return rc;
}

typedef int (*feeder)(csvnc_rfb *, const uint8_t *, size_t);
static const feeder feeders[] = { feed_all, feed_bytewise, feed_chunked };
#define NFEEDERS 3

/* ---- handshake ---- */

static const uint8_t v38[] = "RFB 003.008\n";
static const uint8_t v37[] = "RFB 003.007\n";
static const uint8_t v33[] = "RFB 003.003\n";

static void check_server_init(rec *r, size_t at)
{
    const uint8_t *p = r->out + at;
    csvnc_pixfmt native;
    uint8_t wire[16];

    csvnc_pixfmt_native(&native);
    csvnc_pixfmt_write(&native, wire);
    TCHECK_EQ(r->outlen, at + 24 + 12);
    TCHECK_EQ(csvnc_get_u16(p), 640);
    TCHECK_EQ(csvnc_get_u16(p + 2), 480);
    TCHECK(memcmp(p + 4, wire, 16) == 0);
    TCHECK_EQ(p[4], 32);
    TCHECK_EQ(p[5], 24);
    TCHECK_EQ(p[6], 0);
    TCHECK_EQ(p[7], 1);
    TCHECK_EQ(csvnc_get_u32(p + 20), 12);
    TCHECK(memcmp(p + 24, "EACS console", 12) == 0);
}

static void test_handshake_38(void)
{
    int fi;

    for (fi = 0; fi < NFEEDERS; fi++) {
        csvnc_rfb c;
        rec r;
        uint8_t in[14];

        setup(&c, &r);
        csvnc_rfb_start(&c);
        TCHECK_EQ(r.outlen, 12);
        TCHECK(memcmp(r.out, "RFB 003.008\n", 12) == 0);
        memcpy(in, v38, 12);
        in[12] = 1;     /* security type None */
        in[13] = 1;     /* ClientInit shared */
        TCHECK_EQ(feeders[fi](&c, in, 14), 0);
        TCHECK(csvnc_rfb_established(&c));
        TCHECK_EQ(c.version, 38);
        /* version(12) + [1,1](2) + SecurityResult(4) + ServerInit */
        TCHECK_EQ(r.out[12], 1);
        TCHECK_EQ(r.out[13], 1);
        TCHECK_EQ(csvnc_get_u32(r.out + 14), 0);
        check_server_init(&r, 18);
    }
}

static void test_handshake_37(void)
{
    csvnc_rfb c;
    rec r;
    uint8_t in[14];

    setup(&c, &r);
    csvnc_rfb_start(&c);
    memcpy(in, v37, 12);
    in[12] = 1;
    in[13] = 0;
    TCHECK_EQ(feed_bytewise(&c, in, 14), 0);
    TCHECK(csvnc_rfb_established(&c));
    TCHECK_EQ(c.version, 37);
    TCHECK_EQ(r.out[12], 1);
    TCHECK_EQ(r.out[13], 1);
    /* no SecurityResult in 3.7 for None */
    check_server_init(&r, 14);
}

static void test_handshake_33(void)
{
    csvnc_rfb c;
    rec r;
    uint8_t in[13];

    setup(&c, &r);
    csvnc_rfb_start(&c);
    memcpy(in, v33, 12);
    in[12] = 1;                 /* straight to ClientInit */
    TCHECK_EQ(feed_chunked(&c, in, 13), 0);
    TCHECK(csvnc_rfb_established(&c));
    TCHECK_EQ(c.version, 33);
    TCHECK_EQ(csvnc_get_u32(r.out + 12), 1);   /* security type None */
    check_server_init(&r, 16);
}

static void test_handshake_odd_versions(void)
{
    static const struct { const char *v; int expect; } cases[] = {
        { "RFB 003.005\n", 33 }, { "RFB 003.006\n", 33 },
        { "RFB 003.009\n", 38 }, { "RFB 003.889\n", 38 },
        { "RFB 003.000\n", 33 },
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        csvnc_rfb c;
        rec r;

        setup(&c, &r);
        TCHECK_EQ(feed_all(&c, (const uint8_t *)cases[i].v, 12), 0);
        TCHECK_EQ(c.version, cases[i].expect);
    }
}

static void test_handshake_bad_version(void)
{
    static const char *bad[] = {
        "RFB 004.000\n", "RFB 002.008\n", "RFB 003.00x\n", "RFB 003.008 ",
        "rfb 003.008\n", "RFB 003:008\n", "GET / HTTP/1\n", "\0\0\0\0\0\0\0\0\0\0\0\0",
    };
    size_t i;

    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        csvnc_rfb c;
        rec r;

        setup(&c, &r);
        TCHECK_EQ(feed_bytewise(&c, (const uint8_t *)bad[i], 12), -1);
        TCHECK(!csvnc_rfb_established(&c));
        TCHECK_EQ(c.state, CSVNC_ST_FAILED);
        /* nothing beyond the type list / security type was sent */
        TCHECK_EQ(r.outlen, 0);
        /* a failed parser stays failed */
        TCHECK_EQ(feed_all(&c, (const uint8_t *)"\1\1", 2), -1);
    }
}

static void test_handshake_bad_security(void)
{
    csvnc_rfb c;
    rec r;
    uint8_t in[13];

    /* 3.8: SecurityResult failure with a reason, then dropped */
    setup(&c, &r);
    memcpy(in, v38, 12);
    in[12] = 2;   /* VNC auth */
    TCHECK_EQ(feed_all(&c, in, 13), -1);
    TCHECK_EQ(r.outlen, 2 + 4 + 4 + strlen("unsupported security type"));
    TCHECK_EQ(csvnc_get_u32(r.out + 2), 1);
    TCHECK_EQ(csvnc_get_u32(r.out + 6), strlen("unsupported security type"));
    TCHECK(memcmp(r.out + 10, "unsupported security type", 25) == 0);

    /* 3.7: just dropped */
    setup(&c, &r);
    memcpy(in, v37, 12);
    in[12] = 0;
    TCHECK_EQ(feed_all(&c, in, 13), -1);
    TCHECK_EQ(r.outlen, 2);

    /* a client that skips security and sends ClientInit + a message
     * as its "security type" (0/1 confusion) is dropped too */
    setup(&c, &r);
    memcpy(in, v38, 12);
    in[12] = 3;
    TCHECK_EQ(feed_all(&c, in, 13), -1);
}

/* ---- messages ---- */

static void established(csvnc_rfb *c, rec *r)
{
    uint8_t in[14];

    setup(c, r);
    csvnc_rfb_start(c);
    memcpy(in, v38, 12);
    in[12] = 1;
    in[13] = 1;
    TCHECK_EQ(feed_all(c, in, 14), 0);
    TCHECK(csvnc_rfb_established(c));
    r->loglen = 0;
    r->log[0] = 0;
}

static const uint8_t msg_set_pixel_format[] = {
    0, 0, 0, 0,
    32, 24, 0, 1, 0, 255, 0, 255, 0, 255, 0, 8, 16, 0, 0, 0
};
static const uint8_t msg_set_encodings[] = {
    2, 0, 0, 5,
    0xff, 0xff, 0xff, 0xff,     /* -1: unsupported */
    0x00, 0x00, 0x00, 0x10,     /* ZRLE: the first supported one wins */
    0x00, 0x00, 0x00, 0x05,     /* Hextile */
    0xff, 0xff, 0xff, 0x20,     /* LastRect (-224) */
    0x00, 0x00, 0x00, 0x00      /* Raw */
};
static const uint8_t msg_update_request[] = {
    3, 1, 0x00, 0x0a, 0x00, 0x14, 0x02, 0x80, 0x01, 0xe0
};
static const uint8_t msg_key_down[] = { 4, 1, 0, 0, 0x00, 0x00, 0xff, 0x0d };
static const uint8_t msg_key_up[] = { 4, 0, 0, 0, 0x00, 0x00, 0x00, 0x61 };
static const uint8_t msg_pointer[] = { 5, 0x05, 0x01, 0x00, 0x00, 0xc8 };
static const uint8_t msg_cut_text[] = { 6, 0, 0, 0, 0, 0, 0, 5, 'h', 'e', 'l', 'l', 'o' };
static const uint8_t msg_cut_text_empty[] = { 6, 0, 0, 0, 0, 0, 0, 0 };
static const uint8_t msg_encodings_empty[] = { 2, 0, 0, 0 };

static void test_messages_all(void)
{
    uint8_t stream[256];
    size_t n = 0;
    int fi;

#define APPEND(m) do { memcpy(stream + n, m, sizeof(m)); n += sizeof(m); } while (0)
    APPEND(msg_set_pixel_format);
    APPEND(msg_set_encodings);
    APPEND(msg_update_request);
    APPEND(msg_key_down);
    APPEND(msg_cut_text);
    APPEND(msg_key_up);
    APPEND(msg_pointer);
    APPEND(msg_cut_text_empty);
    APPEND(msg_encodings_empty);
    APPEND(msg_update_request);
#undef APPEND

    for (fi = 0; fi < NFEEDERS; fi++) {
        csvnc_rfb c;
        rec r;

        established(&c, &r);
        TCHECK_EQ(feeders[fi](&c, stream, n), 0);
        TCHECK(csvnc_rfb_established(&c));
        if (strcmp(r.log,
                   "pf(32,24,0,255/255/255,0/8/16);"
                   "enc(16,lr=1);"
                   "fbur(1,10,20,640,480);"
                   "key(ff0d,1);"
                   "key(61,0);"
                   "ptr(256,200,5);"
                   "enc(0,lr=0);"
                   "fbur(1,10,20,640,480);") != 0)
            TFAIL("feeder %d log: %s", fi, r.log);
        /* nothing is sent in response to client messages */
        TCHECK_EQ(r.outlen, 18 + 36);
        TCHECK_EQ(c.enc, CSVNC_ENC_RAW);
        TCHECK_EQ(c.fmt.rshift, 0);
        TCHECK_EQ(c.fmt.bshift, 16);
        TCHECK(c.fmt.host_order != 0 || c.fmt.big_endian);
    }
}

static void test_set_encodings_preference(void)
{
    /* Raw listed before Hextile: the client's order wins */
    static const uint8_t raw_first[] = {
        2, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 5
    };
    /* nothing we support (Tight, Zlib): Raw */
    static const uint8_t none[] = {
        2, 0, 0, 2, 0, 0, 0, 7, 0, 0, 0, 6
    };
    csvnc_rfb c;
    rec r;

    established(&c, &r);
    TCHECK_EQ(feed_bytewise(&c, raw_first, sizeof(raw_first)), 0);
    TCHECK_EQ(c.enc, CSVNC_ENC_RAW);
    TCHECK_EQ(feed_bytewise(&c, msg_set_encodings, sizeof(msg_set_encodings)), 0);
    TCHECK_EQ(c.enc, CSVNC_ENC_ZRLE);
    TCHECK_EQ(c.lastrect, 1);
    TCHECK_EQ(feed_bytewise(&c, none, sizeof(none)), 0);
    TCHECK_EQ(c.enc, CSVNC_ENC_RAW);
    TCHECK_EQ(c.lastrect, 0);
}

static void test_set_encodings_long(void)
{
    /* the maximum count streams through without buffering: 65535
     * encodings, Hextile last */
    csvnc_rfb c;
    rec r;
    uint8_t hdr[4] = { 2, 0, 0xff, 0xff };
    uint8_t enc[4] = { 0, 0, 0, 7 };
    uint8_t hex[4] = { 0, 0, 0, 5 };
    unsigned i;

    established(&c, &r);
    TCHECK_EQ(feed_all(&c, hdr, 4), 0);
    for (i = 0; i < 65534; i++)
        TCHECK_EQ(feed_all(&c, enc, 4), 0);
    TCHECK_EQ(c.state, CSVNC_ST_ENCODINGS);
    TCHECK_EQ(feed_all(&c, hex, 4), 0);
    TCHECK_EQ(c.enc, CSVNC_ENC_HEXTILE);
    TCHECK_EQ(c.state, CSVNC_ST_MSG_TYPE);
    TCHECK(strcmp(r.log, "enc(5,lr=0);") == 0);
}

static void test_bad_pixel_format(void)
{
    static const uint8_t colour_map[] = {
        0, 0, 0, 0, 8, 8, 0, 0, 0, 7, 0, 7, 0, 3, 5, 2, 0, 0, 0, 0
    };
    static const uint8_t bpp24[] = {
        0, 0, 0, 0, 24, 24, 0, 1, 0, 255, 0, 255, 0, 255, 16, 8, 0, 0, 0, 0
    };
    static const uint8_t shift_overflow[] = {
        0, 0, 0, 0, 16, 16, 0, 1, 0, 31, 0, 63, 0, 31, 11, 5, 12, 0, 0, 0
    };
    csvnc_rfb c;
    rec r;

    established(&c, &r);
    TCHECK_EQ(feed_chunked(&c, colour_map, sizeof(colour_map)), -1);
    established(&c, &r);
    TCHECK_EQ(feed_chunked(&c, bpp24, sizeof(bpp24)), -1);
    established(&c, &r);
    TCHECK_EQ(feed_chunked(&c, shift_overflow, sizeof(shift_overflow)), -1);
    /* the format in effect is untouched by a rejected one */
    TCHECK_EQ(c.fmt.bpp, 32);
    TCHECK_EQ(r.loglen, 0);
}

static void test_cut_text_lengths(void)
{
    csvnc_rfb c;
    rec r;
    uint8_t hdr[8] = { 6, 0, 0, 0, 0, 0x10, 0, 0 };   /* 1 MiB exactly */
    uint8_t chunk[4096];
    size_t left = 1u << 20;

    memset(chunk, 'x', sizeof(chunk));
    established(&c, &r);
    TCHECK_EQ(feed_all(&c, hdr, 8), 0);
    while (left) {
        size_t n = left < sizeof(chunk) ? left : sizeof(chunk);

        TCHECK_EQ(feed_all(&c, chunk, n), 0);
        left -= n;
    }
    TCHECK_EQ(c.state, CSVNC_ST_MSG_TYPE);
    TCHECK_EQ(feed_all(&c, msg_key_down, sizeof(msg_key_down)), 0);
    TCHECK(strcmp(r.log, "key(ff0d,1);") == 0);

    /* one byte more is rejected before any payload arrives */
    hdr[7] = 1;
    established(&c, &r);
    TCHECK_EQ(feed_all(&c, hdr, 8), -1);

    /* extended-clipboard negative length: rejected */
    hdr[4] = 0xff;
    hdr[5] = 0xff;
    hdr[6] = 0xff;
    hdr[7] = 0xfb;
    established(&c, &r);
    TCHECK_EQ(feed_bytewise(&c, hdr, 8), -1);

    /* cut text bytes are consumed exactly: the byte after the payload
     * is the next message type, even in one buffer */
    {
        uint8_t both[sizeof(msg_cut_text) + sizeof(msg_pointer)];

        memcpy(both, msg_cut_text, sizeof(msg_cut_text));
        memcpy(both + sizeof(msg_cut_text), msg_pointer, sizeof(msg_pointer));
        established(&c, &r);
        TCHECK_EQ(feed_all(&c, both, sizeof(both)), 0);
        TCHECK(strcmp(r.log, "ptr(256,200,5);") == 0);
    }
}

static void test_unknown_message(void)
{
    uint8_t types[] = { 1, 7, 8, 127, 128, 250, 255 };
    size_t i;

    for (i = 0; i < sizeof(types); i++) {
        csvnc_rfb c;
        rec r;

        established(&c, &r);
        TCHECK_EQ(feed_all(&c, &types[i], 1), -1);
        TCHECK_EQ(r.loglen, 0);
    }
}

static void test_junk_never_reads_past(void)
{
    /* random streams: every feed call is bounded by its length, the
     * parser either fails or stays in a sane state, and the version
     * prefix is never echoed */
    int round;

    for (round = 0; round < 500; round++) {
        csvnc_rfb c;
        rec r;
        uint8_t junk[64];
        size_t n = rng() % sizeof(junk), i;
        int rc;

        for (i = 0; i < n; i++)
            junk[i] = (uint8_t)rng();
        if (round & 1)
            established(&c, &r);
        else
            setup(&c, &r);
        rc = feeders[round % NFEEDERS](&c, junk, n);
        TCHECK(rc == 0 || rc == -1);
        TCHECK(c.state >= CSVNC_ST_VERSION && c.state <= CSVNC_ST_FAILED);
        TCHECK(c.have <= c.need);
        TCHECK(c.need <= sizeof(c.buf));
    }
}

static void test_message_before_handshake(void)
{
    /* a message-typed byte during the handshake is a bad version */
    csvnc_rfb c;
    rec r;

    setup(&c, &r);
    TCHECK_EQ(feed_all(&c, msg_key_down, sizeof(msg_key_down)), 0);   /* 8 < 12 */
    TCHECK_EQ(c.state, CSVNC_ST_VERSION);
    TCHECK_EQ(feed_all(&c, msg_key_down, sizeof(msg_key_down)), -1);
    TCHECK_EQ(r.loglen, 0);
}

TEST_MAIN(test_handshake_38, test_handshake_37, test_handshake_33,
          test_handshake_odd_versions, test_handshake_bad_version,
          test_handshake_bad_security, test_messages_all,
          test_set_encodings_preference, test_set_encodings_long,
          test_bad_pixel_format, test_cut_text_lengths, test_unknown_message,
          test_junk_never_reads_past, test_message_before_handshake)
