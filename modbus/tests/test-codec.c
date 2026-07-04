/* Codec tests: CRC16 (vs a bit-by-bit reference + known vectors), every
 * PDU builder round-tripped through its decoder, and the MBAP/RTU frame
 * parsers driven with splits at every byte boundary. */

#include "../csmb-private.h"
#include "test-util.h"

/* ---- CRC16 ---- */

/* Straight bit-by-bit reference (poly 0xA001, init 0xFFFF), independent
 * of the table-driven implementation under test. */
static uint16_t crc16_ref(const uint8_t *p, size_t len)
{
    uint16_t crc = 0xFFFF;
    size_t i;
    int b;

    for (i = 0; i < len; i++) {
        crc ^= p[i];
        for (b = 0; b < 8; b++)
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001)
                            : (uint16_t)(crc >> 1);
    }
    return crc;
}

static void test_crc16_reference(void)
{
    uint8_t buf[256];
    uint32_t seed = 0x12345678u;
    int t, i;

    for (t = 0; t < 500; t++) {
        int len = (int)(seed % 200) + 1;

        for (i = 0; i < len; i++) {
            seed = seed * 1103515245u + 12345u;
            buf[i] = (uint8_t)(seed >> 16);
        }
        TCHECK_EQ(csmb_crc16(buf, (size_t)len), crc16_ref(buf, (size_t)len));
    }
}

static void test_crc16_known(void)
{
    /* Well-known Modbus vectors, transmitted low byte first:
     *   request  01 03 00 00 00 0A -> CRC C5 CD
     *   response 01 04 02 FF FF    -> CRC B8 80
     * Both are cross-checked against crc16_ref() so a wrong literal here
     * would surface as the reference disagreeing with the constant. */
    const uint8_t f1[] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x0A };
    const uint8_t f2[] = { 0x01, 0x04, 0x02, 0xFF, 0xFF };
    uint16_t c1 = csmb_crc16(f1, sizeof f1);
    uint16_t c2 = csmb_crc16(f2, sizeof f2);

    TCHECK_EQ(c1, crc16_ref(f1, sizeof f1));
    TCHECK_EQ(c2, crc16_ref(f2, sizeof f2));
    TCHECK_EQ(c1 & 0xFF, 0xC5);
    TCHECK_EQ(c1 >> 8, 0xCD);
    TCHECK_EQ(c2 & 0xFF, 0xB8);
    TCHECK_EQ(c2 >> 8, 0x80);
}

/* ---- builders round-tripped through decoders ---- */

static void test_build_read(void)
{
    uint8_t pdu[8];
    csmb_request req;
    int i, n;
    struct { int rt; uint8_t fc; } cases[] = {
        { CSMB_COIL,     CSMB_FC_READ_COILS    },
        { CSMB_DISCRETE, CSMB_FC_READ_DISCRETE },
        { CSMB_HOLDING,  CSMB_FC_READ_HOLDING  },
        { CSMB_INPUT,    CSMB_FC_READ_INPUT    },
    };

    for (i = 0; i < 4; i++) {
        n = csmb_build_read(pdu, cases[i].rt, 0x1234, 10);
        TCHECK_EQ(n, 5);
        TCHECK_EQ(pdu[0], cases[i].fc);
        TCHECK_EQ(csmb_decode_request(pdu, (size_t)n, &req), CSMB_OK);
        TCHECK_EQ(req.fc, cases[i].fc);
        TCHECK_EQ(req.reg_type, cases[i].rt);
        TCHECK_EQ(req.start, 0x1234);
        TCHECK_EQ(req.count, 10);
        TCHECK_EQ(req.exc, CSMB_EXC_NONE);
    }
    /* limits and bad reg type */
    TCHECK(csmb_build_read(pdu, CSMB_HOLDING, 0, CSMB_MAX_READ_WORDS + 1) < 0);
    TCHECK_EQ(csmb_build_read(pdu, CSMB_HOLDING, 0, CSMB_MAX_READ_WORDS), 5);
    TCHECK(csmb_build_read(pdu, CSMB_COIL, 0, CSMB_MAX_READ_FLAGS + 1) < 0);
    TCHECK(csmb_build_read(pdu, CSMB_COIL, 0, 0) < 0);
    TCHECK(csmb_build_read(pdu, 99, 0, 1) < 0);
}

static void test_build_write_single(void)
{
    uint8_t pdu[8];
    csmb_request req;

    /* coil on -> 0xFF00 */
    TCHECK_EQ(csmb_build_write_single(pdu, CSMB_COIL, 0x0013, 1), 5);
    TCHECK_EQ(pdu[0], CSMB_FC_WRITE_SINGLE_COIL);
    TCHECK_EQ(pdu[3], 0xFF);
    TCHECK_EQ(pdu[4], 0x00);
    TCHECK_EQ(csmb_decode_request(pdu, 5, &req), CSMB_OK);
    TCHECK_EQ(req.reg_type, CSMB_COIL);
    TCHECK_EQ(req.start, 0x0013);
    TCHECK_EQ(req.count, 1);
    TCHECK_EQ(req.value, 1);
    TCHECK_EQ(req.exc, CSMB_EXC_NONE);

    /* coil off -> 0x0000 */
    TCHECK_EQ(csmb_build_write_single(pdu, CSMB_COIL, 0x0013, 0), 5);
    TCHECK_EQ(pdu[3], 0x00);
    TCHECK_EQ(pdu[4], 0x00);
    TCHECK_EQ(csmb_decode_request(pdu, 5, &req), CSMB_OK);
    TCHECK_EQ(req.value, 0);

    /* holding, raw value */
    TCHECK_EQ(csmb_build_write_single(pdu, CSMB_HOLDING, 0x0100, 0xABCD), 5);
    TCHECK_EQ(pdu[0], CSMB_FC_WRITE_SINGLE_REG);
    TCHECK_EQ(csmb_decode_request(pdu, 5, &req), CSMB_OK);
    TCHECK_EQ(req.reg_type, CSMB_HOLDING);
    TCHECK_EQ(req.value, 0xABCD);

    /* discrete/input are not writable */
    TCHECK(csmb_build_write_single(pdu, CSMB_DISCRETE, 0, 1) < 0);
    TCHECK(csmb_build_write_single(pdu, CSMB_INPUT, 0, 1) < 0);
}

static void test_build_write_multi(void)
{
    uint8_t pdu[300];
    csmb_request req;
    uint16_t coils[20], regs[10];
    int i, n;

    for (i = 0; i < 20; i++)
        coils[i] = (uint16_t)(i % 3 == 0);
    n = csmb_build_write_multi(pdu, CSMB_COIL, 0x0000, 20, coils);
    TCHECK_EQ(n, 6 + 3);          /* (20 + 7) / 8 = 3 */
    TCHECK_EQ(pdu[0], CSMB_FC_WRITE_MULTI_COILS);
    TCHECK_EQ(pdu[5], 3);
    TCHECK_EQ(csmb_decode_request(pdu, (size_t)n, &req), CSMB_OK);
    TCHECK_EQ(req.reg_type, CSMB_COIL);
    TCHECK_EQ(req.start, 0);
    TCHECK_EQ(req.count, 20);
    TCHECK_EQ(req.nbytes, 3);
    TCHECK_EQ(req.exc, CSMB_EXC_NONE);
    for (i = 0; i < 20; i++) {
        int bit = (req.data[i / 8] >> (i % 8)) & 1;
        TCHECK_EQ(bit, (i % 3 == 0));
    }

    for (i = 0; i < 10; i++)
        regs[i] = (uint16_t)(0x1000 + i);
    n = csmb_build_write_multi(pdu, CSMB_HOLDING, 0x0040, 10, regs);
    TCHECK_EQ(n, 6 + 20);
    TCHECK_EQ(pdu[0], CSMB_FC_WRITE_MULTI_REGS);
    TCHECK_EQ(csmb_decode_request(pdu, (size_t)n, &req), CSMB_OK);
    TCHECK_EQ(req.reg_type, CSMB_HOLDING);
    TCHECK_EQ(req.count, 10);
    TCHECK_EQ(req.nbytes, 20);
    for (i = 0; i < 10; i++) {
        uint16_t v = (uint16_t)((req.data[2 * i] << 8) | req.data[2 * i + 1]);
        TCHECK_EQ(v, 0x1000 + i);
    }

    /* limits and bad reg type */
    TCHECK(csmb_build_write_multi(pdu, CSMB_HOLDING, 0,
                                  CSMB_MAX_WRITE_WORDS + 1, regs) < 0);
    TCHECK(csmb_build_write_multi(pdu, CSMB_COIL, 0,
                                  CSMB_MAX_WRITE_COILS + 1, coils) < 0);
    TCHECK(csmb_build_write_multi(pdu, CSMB_INPUT, 0, 1, regs) < 0);
}

static void test_build_responses(void)
{
    uint8_t pdu[300];
    csmb_response resp;
    uint16_t regs[5] = { 0x1111, 0x2222, 0x3333, 0x4444, 0x5555 };
    uint16_t coils[10];
    int i, n;

    /* read-holding response */
    n = csmb_build_read_response(pdu, CSMB_FC_READ_HOLDING, regs, 5, 0);
    TCHECK_EQ(n, 2 + 10);
    TCHECK_EQ(pdu[1], 10);
    TCHECK_EQ(csmb_decode_response(pdu, (size_t)n, CSMB_FC_READ_HOLDING, &resp),
              CSMB_OK);
    TCHECK_EQ(resp.fc, CSMB_FC_READ_HOLDING);
    TCHECK_EQ(resp.exception, 0);
    TCHECK_EQ(resp.nbytes, 10);
    for (i = 0; i < 5; i++) {
        uint16_t v = (uint16_t)((resp.data[2 * i] << 8) | resp.data[2 * i + 1]);
        TCHECK_EQ(v, regs[i]);
    }

    /* read-coils response, bit-packed */
    for (i = 0; i < 10; i++)
        coils[i] = (uint16_t)(i & 1);
    n = csmb_build_read_response(pdu, CSMB_FC_READ_COILS, coils, 10, 1);
    TCHECK_EQ(n, 2 + 2);          /* (10 + 7) / 8 = 2 */
    TCHECK_EQ(csmb_decode_response(pdu, (size_t)n, CSMB_FC_READ_COILS, &resp),
              CSMB_OK);
    TCHECK_EQ(resp.nbytes, 2);
    for (i = 0; i < 10; i++) {
        int bit = (resp.data[i / 8] >> (i % 8)) & 1;
        TCHECK_EQ(bit, (i & 1));
    }

    /* write echo (FC16) */
    n = csmb_build_write_response(pdu, CSMB_FC_WRITE_MULTI_REGS, 0x0040, 10);
    TCHECK_EQ(n, 5);
    TCHECK_EQ(csmb_decode_response(pdu, 5, CSMB_FC_WRITE_MULTI_REGS, &resp),
              CSMB_OK);
    TCHECK_EQ(resp.start, 0x0040);
    TCHECK_EQ(resp.count_or_value, 10);

    /* write echo (FC6) */
    n = csmb_build_write_response(pdu, CSMB_FC_WRITE_SINGLE_REG, 0x0100, 0xABCD);
    TCHECK_EQ(csmb_decode_response(pdu, 5, CSMB_FC_WRITE_SINGLE_REG, &resp),
              CSMB_OK);
    TCHECK_EQ(resp.start, 0x0100);
    TCHECK_EQ(resp.count_or_value, 0xABCD);

    /* exception */
    n = csmb_build_exception(pdu, CSMB_FC_READ_HOLDING, CSMB_EXC_ILLEGAL_ADDRESS);
    TCHECK_EQ(n, 2);
    TCHECK_EQ(pdu[0], CSMB_FC_READ_HOLDING | 0x80);
    TCHECK_EQ(csmb_decode_response(pdu, 2, CSMB_FC_READ_HOLDING, &resp), CSMB_OK);
    TCHECK_EQ(resp.fc, CSMB_FC_READ_HOLDING | 0x80);
    TCHECK_EQ(resp.exception, CSMB_EXC_ILLEGAL_ADDRESS);

    /* fc mismatch on a normal response */
    n = csmb_build_write_response(pdu, CSMB_FC_WRITE_MULTI_REGS, 0, 1);
    TCHECK(csmb_decode_response(pdu, 5, CSMB_FC_WRITE_MULTI_COILS, &resp) < 0);
}

/* ---- MBAP parser ---- */

static size_t make_mbap(uint8_t *buf, uint16_t tid, uint8_t unit,
                        const uint8_t *pdu, size_t plen)
{
    int total;

    memcpy(buf + CSMB_MBAP_LEN, pdu, plen);
    total = csmb_mbap_wrap(buf, tid, unit, plen);
    TCHECK(total > 0);
    return (size_t)total;
}

/* Parse FRAME as a single call and again split at every interior byte
 * position, asserting the same result each time. */
static void mbap_check_frame(const uint8_t *frame, size_t flen,
                             uint16_t tid, uint8_t unit,
                             const uint8_t *pdu, uint16_t plen)
{
    csmb_mbap_parser p;
    size_t split;
    const uint8_t *in;
    size_t n;

    in = frame;
    n = flen;
    csmb_mbap_parser_init(&p);
    TCHECK_EQ(csmb_mbap_parser_feed(&p, &in, &n), CSMB_PR_FRAME);
    TCHECK_EQ(n, 0);
    TCHECK_EQ(p.tid, tid);
    TCHECK_EQ(p.unit, unit);
    TCHECK_EQ(p.plen, plen);
    TCHECK(memcmp(p.pdu, pdu, plen) == 0);

    for (split = 1; split < flen; split++) {
        csmb_mbap_parser_init(&p);
        in = frame;
        n = split;
        TCHECK_EQ(csmb_mbap_parser_feed(&p, &in, &n), CSMB_PR_NEED_MORE);
        TCHECK_EQ(n, 0);
        in = frame + split;
        n = flen - split;
        TCHECK_EQ(csmb_mbap_parser_feed(&p, &in, &n), CSMB_PR_FRAME);
        TCHECK_EQ(n, 0);
        TCHECK_EQ(p.tid, tid);
        TCHECK_EQ(p.unit, unit);
        TCHECK_EQ(p.plen, plen);
        TCHECK(memcmp(p.pdu, pdu, plen) == 0);
    }
}

static void test_mbap_frames(void)
{
    uint8_t pdu[300], frame[320];
    size_t flen;
    int plen, i;
    uint16_t regs[125];

    /* read request FC3 */
    plen = csmb_build_read(pdu, CSMB_HOLDING, 0x0064, 5);
    flen = make_mbap(frame, 0x1234, 0x11, pdu, (size_t)plen);
    mbap_check_frame(frame, flen, 0x1234, 0x11, pdu, (uint16_t)plen);

    /* read response FC3 */
    for (i = 0; i < 5; i++)
        regs[i] = (uint16_t)(i + 1);
    plen = csmb_build_read_response(pdu, CSMB_FC_READ_HOLDING, regs, 5, 0);
    flen = make_mbap(frame, 0x0002, 0x11, pdu, (size_t)plen);
    mbap_check_frame(frame, flen, 0x0002, 0x11, pdu, (uint16_t)plen);

    /* exception */
    plen = csmb_build_exception(pdu, CSMB_FC_READ_HOLDING, CSMB_EXC_SERVER_FAILURE);
    flen = make_mbap(frame, 0x0003, 0x22, pdu, (size_t)plen);
    mbap_check_frame(frame, flen, 0x0003, 0x22, pdu, (uint16_t)plen);

    /* near-max PDU (125 words -> 252 bytes) exercises the length boundary */
    for (i = 0; i < 125; i++)
        regs[i] = (uint16_t)(i * 7 + 1);
    plen = csmb_build_read_response(pdu, CSMB_FC_READ_HOLDING, regs, 125, 0);
    TCHECK_EQ(plen, 252);
    flen = make_mbap(frame, 0x0004, 0x01, pdu, (size_t)plen);
    mbap_check_frame(frame, flen, 0x0004, 0x01, pdu, (uint16_t)plen);
}

static void test_mbap_three_part(void)
{
    uint8_t pdu[16], frame[32];
    size_t flen;
    int plen;
    csmb_mbap_parser p;
    const uint8_t *in;
    size_t n;

    plen = csmb_build_read(pdu, CSMB_HOLDING, 0x0010, 8);
    flen = make_mbap(frame, 0x0055, 0x09, pdu, (size_t)plen);   /* flen == 12 */

    csmb_mbap_parser_init(&p);
    in = frame; n = 3;
    TCHECK_EQ(csmb_mbap_parser_feed(&p, &in, &n), CSMB_PR_NEED_MORE);
    in = frame + 3; n = 3;
    TCHECK_EQ(csmb_mbap_parser_feed(&p, &in, &n), CSMB_PR_NEED_MORE);
    in = frame + 6; n = flen - 6;
    TCHECK_EQ(csmb_mbap_parser_feed(&p, &in, &n), CSMB_PR_FRAME);
    TCHECK_EQ(n, 0);
    TCHECK_EQ(p.tid, 0x0055);
    TCHECK_EQ(p.unit, 0x09);
    TCHECK_EQ(p.plen, plen);
    TCHECK(memcmp(p.pdu, pdu, (size_t)plen) == 0);
}

static void test_mbap_back_to_back(void)
{
    uint8_t pdu[16], f1[32], f2[32], buf[64];
    size_t l1, l2;
    int plen1, plen2;
    csmb_mbap_parser p;
    const uint8_t *in;
    size_t n;

    plen1 = csmb_build_read(pdu, CSMB_HOLDING, 0x0000, 3);
    l1 = make_mbap(f1, 0x000A, 0x01, pdu, (size_t)plen1);
    plen2 = csmb_build_read(pdu, CSMB_INPUT, 0x0010, 4);
    l2 = make_mbap(f2, 0x000B, 0x02, pdu, (size_t)plen2);
    memcpy(buf, f1, l1);
    memcpy(buf + l1, f2, l2);

    csmb_mbap_parser_init(&p);
    in = buf;
    n = l1 + l2;
    TCHECK_EQ(csmb_mbap_parser_feed(&p, &in, &n), CSMB_PR_FRAME);
    TCHECK_EQ(n, l2);            /* remainder stays for the next frame */
    TCHECK_EQ(p.tid, 0x000A);
    TCHECK_EQ(p.unit, 0x01);

    csmb_mbap_parser_reset(&p);
    TCHECK_EQ(csmb_mbap_parser_feed(&p, &in, &n), CSMB_PR_FRAME);
    TCHECK_EQ(n, 0);
    TCHECK_EQ(p.tid, 0x000B);
    TCHECK_EQ(p.unit, 0x02);
}

static void test_mbap_bad(void)
{
    csmb_mbap_parser p;
    uint8_t hdr[CSMB_MBAP_LEN];
    const uint8_t *in;
    size_t n;
    int k;
    /* {proto_hi, proto_lo, len_hi, len_lo} variations that must be rejected */
    struct { uint8_t a, b, c, d; } bad[] = {
        { 0x00, 0x01, 0x00, 0x06 },  /* protocol id != 0 */
        { 0x00, 0x00, 0x00, 0x00 },  /* length 0 */
        { 0x00, 0x00, 0x00, 0x01 },  /* length 1 */
        { 0x00, 0x00, 0x00, 0xFF },  /* length 255 (> 254) */
        { 0x00, 0x00, 0x01, 0x2C },  /* length 300 */
    };

    for (k = 0; k < 5; k++) {
        memset(hdr, 0, sizeof hdr);
        hdr[2] = bad[k].a;
        hdr[3] = bad[k].b;
        hdr[4] = bad[k].c;
        hdr[5] = bad[k].d;
        hdr[6] = 0x01;
        csmb_mbap_parser_init(&p);
        in = hdr;
        n = sizeof hdr;
        TCHECK_EQ(csmb_mbap_parser_feed(&p, &in, &n), CSMB_PR_BAD);
    }
}

/* ---- RTU parser ---- */

static size_t make_rtu(uint8_t *buf, uint8_t unit, const uint8_t *pdu,
                       size_t plen)
{
    int total;

    memcpy(buf + 1, pdu, plen);
    total = csmb_rtu_wrap(buf, unit, plen);
    TCHECK(total > 0);
    return (size_t)total;
}

static void rtu_check_frame(int slave_mode, const uint8_t *frame, size_t flen,
                            uint8_t unit, const uint8_t *pdu, uint16_t plen)
{
    csmb_rtu_parser p;
    size_t split;
    const uint8_t *in;
    size_t n;

    in = frame;
    n = flen;
    csmb_rtu_parser_init(&p, slave_mode);
    TCHECK_EQ(csmb_rtu_parser_feed(&p, &in, &n), CSMB_PR_FRAME);
    TCHECK_EQ(n, 0);
    TCHECK_EQ(p.unit, unit);
    TCHECK_EQ(p.plen, plen);
    TCHECK(memcmp(p.pdu, pdu, plen) == 0);

    for (split = 1; split < flen; split++) {
        csmb_rtu_parser_init(&p, slave_mode);
        in = frame;
        n = split;
        TCHECK_EQ(csmb_rtu_parser_feed(&p, &in, &n), CSMB_PR_NEED_MORE);
        TCHECK_EQ(n, 0);
        in = frame + split;
        n = flen - split;
        TCHECK_EQ(csmb_rtu_parser_feed(&p, &in, &n), CSMB_PR_FRAME);
        TCHECK_EQ(n, 0);
        TCHECK_EQ(p.unit, unit);
        TCHECK_EQ(p.plen, plen);
        TCHECK(memcmp(p.pdu, pdu, plen) == 0);
    }
}

static void test_rtu_master(void)
{
    uint8_t pdu[300], frame[320];
    size_t flen;
    int plen;
    uint16_t regs[4] = { 0xDEAD, 0xBEEF, 0x0000, 0xFFFF };

    /* FC3 response, variable length via the byte-count field */
    plen = csmb_build_read_response(pdu, CSMB_FC_READ_HOLDING, regs, 4, 0);
    flen = make_rtu(frame, 0x11, pdu, (size_t)plen);
    rtu_check_frame(0, frame, flen, 0x11, pdu, (uint16_t)plen);
    {
        csmb_rtu_parser p;
        const uint8_t *in = frame;
        size_t n = flen;
        csmb_response resp;

        csmb_rtu_parser_init(&p, 0);
        TCHECK_EQ(csmb_rtu_parser_feed(&p, &in, &n), CSMB_PR_FRAME);
        TCHECK_EQ(csmb_decode_response(p.pdu, p.plen, CSMB_FC_READ_HOLDING,
                                       &resp), CSMB_OK);
        TCHECK_EQ(resp.nbytes, 8);
    }

    /* FC5 echo (fixed 8-byte frame) */
    plen = csmb_build_write_response(pdu, CSMB_FC_WRITE_SINGLE_COIL, 0x0013, 0xFF00);
    flen = make_rtu(frame, 0x11, pdu, (size_t)plen);
    TCHECK_EQ(flen, 8);
    rtu_check_frame(0, frame, flen, 0x11, pdu, (uint16_t)plen);

    /* exception response (5-byte frame) */
    plen = csmb_build_exception(pdu, CSMB_FC_READ_HOLDING, CSMB_EXC_ILLEGAL_ADDRESS);
    flen = make_rtu(frame, 0x11, pdu, (size_t)plen);
    TCHECK_EQ(flen, 5);
    rtu_check_frame(0, frame, flen, 0x11, pdu, (uint16_t)plen);
}

static void test_rtu_slave(void)
{
    uint8_t pdu[300], frame[320];
    size_t flen;
    int plen, i;
    uint16_t regs[3] = { 0x1111, 0x2222, 0x3333 };
    uint16_t coils[20];

    /* FC16 request, variable length via the byte-count field at index 6 */
    plen = csmb_build_write_multi(pdu, CSMB_HOLDING, 0x0040, 3, regs);
    flen = make_rtu(frame, 0x22, pdu, (size_t)plen);
    rtu_check_frame(1, frame, flen, 0x22, pdu, (uint16_t)plen);
    {
        csmb_rtu_parser p;
        const uint8_t *in = frame;
        size_t n = flen;
        csmb_request req;

        csmb_rtu_parser_init(&p, 1);
        TCHECK_EQ(csmb_rtu_parser_feed(&p, &in, &n), CSMB_PR_FRAME);
        TCHECK_EQ(csmb_decode_request(p.pdu, p.plen, &req), CSMB_OK);
        TCHECK_EQ(req.fc, CSMB_FC_WRITE_MULTI_REGS);
        TCHECK_EQ(req.count, 3);
        TCHECK_EQ(req.nbytes, 6);
    }

    /* FC1 read request (fixed 8-byte frame) */
    plen = csmb_build_read(pdu, CSMB_COIL, 0x0000, 16);
    flen = make_rtu(frame, 0x22, pdu, (size_t)plen);
    TCHECK_EQ(flen, 8);
    rtu_check_frame(1, frame, flen, 0x22, pdu, (uint16_t)plen);

    /* FC15 request, byte-count path (20 coils -> 3 data bytes) */
    for (i = 0; i < 20; i++)
        coils[i] = (uint16_t)(i % 2);
    plen = csmb_build_write_multi(pdu, CSMB_COIL, 0x0000, 20, coils);
    flen = make_rtu(frame, 0x22, pdu, (size_t)plen);
    rtu_check_frame(1, frame, flen, 0x22, pdu, (uint16_t)plen);
}

static void test_rtu_bad(void)
{
    uint8_t pdu[64], frame[80];
    size_t flen;
    int plen;
    csmb_rtu_parser p;
    const uint8_t *in;
    size_t n;
    uint16_t regs[4] = { 1, 2, 3, 4 };
    uint8_t junk[2] = { 0x00, 0x42 };   /* unit + unknown fc */

    /* corrupt CRC (master parses a response) */
    plen = csmb_build_read_response(pdu, CSMB_FC_READ_HOLDING, regs, 4, 0);
    flen = make_rtu(frame, 0x11, pdu, (size_t)plen);
    frame[flen - 1] ^= 0xFF;
    csmb_rtu_parser_init(&p, 0);
    in = frame;
    n = flen;
    TCHECK_EQ(csmb_rtu_parser_feed(&p, &in, &n), CSMB_PR_BAD);

    /* unknown fc, master and slave */
    csmb_rtu_parser_init(&p, 0);
    in = junk;
    n = 2;
    TCHECK_EQ(csmb_rtu_parser_feed(&p, &in, &n), CSMB_PR_BAD);
    junk[1] = 0x63;
    csmb_rtu_parser_init(&p, 1);
    in = junk;
    n = 2;
    TCHECK_EQ(csmb_rtu_parser_feed(&p, &in, &n), CSMB_PR_BAD);

    /* garbage -> BAD, then reset resyncs and a clean frame parses */
    plen = csmb_build_read_response(pdu, CSMB_FC_READ_HOLDING, regs, 2, 0);
    flen = make_rtu(frame, 0x11, pdu, (size_t)plen);
    csmb_rtu_parser_init(&p, 0);
    junk[1] = 0x42;
    in = junk;
    n = 2;
    TCHECK_EQ(csmb_rtu_parser_feed(&p, &in, &n), CSMB_PR_BAD);
    csmb_rtu_parser_reset(&p);
    in = frame;
    n = flen;
    TCHECK_EQ(csmb_rtu_parser_feed(&p, &in, &n), CSMB_PR_FRAME);
    TCHECK_EQ(p.unit, 0x11);
    TCHECK_EQ(p.plen, plen);
}

/* ---- request-decode validation ---- */

static void test_decode_request_validation(void)
{
    uint8_t pdu[64];
    csmb_request req;
    int plen, i;
    uint16_t coils[10];

    /* count over the read-words limit -> ILLEGAL_VALUE */
    pdu[0] = CSMB_FC_READ_HOLDING;
    pdu[1] = 0x00; pdu[2] = 0x00;
    pdu[3] = 0x00; pdu[4] = 0xC8;          /* count = 200 (> 125) */
    TCHECK_EQ(csmb_decode_request(pdu, 5, &req), CSMB_OK);
    TCHECK_EQ(req.exc, CSMB_EXC_ILLEGAL_VALUE);

    /* count over the read-flags limit -> ILLEGAL_VALUE */
    pdu[0] = CSMB_FC_READ_COILS;
    pdu[3] = 0x07; pdu[4] = 0xD1;          /* count = 2001 (> 2000) */
    TCHECK_EQ(csmb_decode_request(pdu, 5, &req), CSMB_OK);
    TCHECK_EQ(req.exc, CSMB_EXC_ILLEGAL_VALUE);

    /* zero count -> ILLEGAL_VALUE */
    pdu[0] = CSMB_FC_READ_HOLDING;
    pdu[3] = 0x00; pdu[4] = 0x00;
    TCHECK_EQ(csmb_decode_request(pdu, 5, &req), CSMB_OK);
    TCHECK_EQ(req.exc, CSMB_EXC_ILLEGAL_VALUE);

    /* FC5 illegal coil value -> ILLEGAL_VALUE */
    pdu[0] = CSMB_FC_WRITE_SINGLE_COIL;
    pdu[1] = 0x00; pdu[2] = 0x01;
    pdu[3] = 0x12; pdu[4] = 0x34;          /* neither 0x0000 nor 0xFF00 */
    TCHECK_EQ(csmb_decode_request(pdu, 5, &req), CSMB_OK);
    TCHECK_EQ(req.exc, CSMB_EXC_ILLEGAL_VALUE);

    /* FC15 byte count that disagrees with the frame length -> negative */
    for (i = 0; i < 10; i++)
        coils[i] = 1;
    plen = csmb_build_write_multi(pdu, CSMB_COIL, 0, 10, coils);
    pdu[5] = 3;                             /* was 2; len no longer 6+nbytes */
    TCHECK(csmb_decode_request(pdu, (size_t)plen, &req) < 0);

    /* FC16 byte count inconsistent with count, length self-consistent -> neg */
    pdu[0] = CSMB_FC_WRITE_MULTI_REGS;
    pdu[1] = 0x00; pdu[2] = 0x00;
    pdu[3] = 0x00; pdu[4] = 0x04;          /* count = 4 -> nbytes should be 8 */
    pdu[5] = 0x06;                          /* claim 6 */
    memset(pdu + 6, 0, 6);
    TCHECK(csmb_decode_request(pdu, 12, &req) < 0);

    /* unknown fc -> ILLEGAL_FUNCTION */
    pdu[0] = 0x42;
    TCHECK_EQ(csmb_decode_request(pdu, 1, &req), CSMB_OK);
    TCHECK_EQ(req.exc, CSMB_EXC_ILLEGAL_FUNCTION);

    /* truncated read request -> negative */
    pdu[0] = CSMB_FC_READ_HOLDING;
    TCHECK(csmb_decode_request(pdu, 4, &req) < 0);
}

TEST_MAIN(test_crc16_reference, test_crc16_known,
          test_build_read, test_build_write_single, test_build_write_multi,
          test_build_responses,
          test_mbap_frames, test_mbap_three_part, test_mbap_back_to_back,
          test_mbap_bad,
          test_rtu_master, test_rtu_slave, test_rtu_bad,
          test_decode_request_validation)
