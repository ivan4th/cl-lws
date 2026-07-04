/* PDU encode/decode, MBAP + RTU frame parsers, CRC16.
 *
 * Pure logic, no libwebsockets: framing is a resumable byte-feeder so
 * the transport can hand over whatever a read() returned.  See
 * csmb-private.h for the parser contracts. */

#include "csmb-private.h"

/* ---- big-endian scalar helpers ---- */

static uint16_t rd_u16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static void wr_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

/* ---- CRC16 (Modbus): reflected, poly 0xA001, init 0xFFFF ---- */

static uint16_t crc16_table[256];
static int crc16_table_ready;

static void crc16_build_table(void)
{
    int i, j;

    for (i = 0; i < 256; i++) {
        uint16_t crc = (uint16_t)i;

        for (j = 0; j < 8; j++)
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001)
                            : (uint16_t)(crc >> 1);
        crc16_table[i] = crc;
    }
    crc16_table_ready = 1;
}

uint16_t csmb_crc16(const uint8_t *p, size_t len)
{
    uint16_t crc = 0xFFFF;
    size_t i;

    if (!crc16_table_ready)
        crc16_build_table();
    for (i = 0; i < len; i++)
        crc = (uint16_t)((crc >> 8) ^ crc16_table[(crc ^ p[i]) & 0xFF]);
    return crc;
}

/* ---- MBAP (Modbus/TCP) frame parser ---- */

void csmb_mbap_parser_init(csmb_mbap_parser *p)
{
    memset(p, 0, sizeof(*p));
}

void csmb_mbap_parser_reset(csmb_mbap_parser *p)
{
    p->got = 0;
    p->have_header = 0;
}

csmb_parse_ret csmb_mbap_parser_feed(csmb_mbap_parser *p,
                                     const uint8_t **in, size_t *len)
{
    if (!p->have_header) {
        while (p->got < CSMB_MBAP_LEN && *len > 0) {
            p->hdr[p->got++] = *(*in)++;
            (*len)--;
        }
        if (p->got < CSMB_MBAP_LEN)
            return CSMB_PR_NEED_MORE;

        {
            uint16_t proto  = rd_u16(p->hdr + 2);
            uint16_t length = rd_u16(p->hdr + 4);

            /* length covers unit + PDU: 2..254 => PDU 1..253 */
            if (proto != 0 || length < 2 || length > CSMB_MAX_PDU + 1)
                return CSMB_PR_BAD;
            p->tid  = rd_u16(p->hdr);
            p->unit = p->hdr[6];
            p->plen = (uint16_t)(length - 1);
        }
        p->have_header = 1;
        p->got = 0;
    }

    while (p->got < p->plen && *len > 0) {
        p->pdu[p->got++] = *(*in)++;
        (*len)--;
    }
    if (p->got < p->plen)
        return CSMB_PR_NEED_MORE;
    return CSMB_PR_FRAME;
}

/* ---- RTU frame parser ---- */

void csmb_rtu_parser_init(csmb_rtu_parser *p, int slave_mode)
{
    memset(p, 0, sizeof(*p));
    p->slave_mode = slave_mode;
}

void csmb_rtu_parser_reset(csmb_rtu_parser *p)
{
    p->got = 0;
    p->expected = 0;
}

/* Grow p->got up to TARGET from the input, returning nonzero once
 * p->got >= TARGET (else input is exhausted). */
static int rtu_fill_to(csmb_rtu_parser *p, size_t target,
                       const uint8_t **in, size_t *len)
{
    while (p->got < target && *len > 0) {
        p->buf[p->got++] = *(*in)++;
        (*len)--;
    }
    return p->got >= target;
}

csmb_parse_ret csmb_rtu_parser_feed(csmb_rtu_parser *p,
                                    const uint8_t **in, size_t *len)
{
    /* Need unit + fc before the frame length can be derived. */
    if (!rtu_fill_to(p, 2, in, len))
        return CSMB_PR_NEED_MORE;

    if (p->expected == 0) {
        uint8_t fc = p->buf[1];

        if (!p->slave_mode) {
            /* master: parsing responses */
            if (fc & 0x80) {
                p->expected = 5;                 /* unit fc exc + CRC */
            } else if (fc >= 1 && fc <= 4) {
                if (!rtu_fill_to(p, 3, in, len)) /* byte count at index 2 */
                    return CSMB_PR_NEED_MORE;
                p->expected = 3u + p->buf[2] + 2u;
            } else if (fc == 5 || fc == 6 || fc == 15 || fc == 16) {
                p->expected = 8;                 /* echo + CRC */
            } else {
                return CSMB_PR_BAD;
            }
        } else {
            /* slave: parsing requests */
            if (fc >= 1 && fc <= 6) {
                p->expected = 8;
            } else if (fc == 15 || fc == 16) {
                if (!rtu_fill_to(p, 7, in, len)) /* byte count at index 6 */
                    return CSMB_PR_NEED_MORE;
                p->expected = 7u + p->buf[6] + 2u;
            } else {
                return CSMB_PR_BAD;
            }
        }
        if (p->expected > (size_t)(CSMB_MAX_PDU + 3))
            return CSMB_PR_BAD;
    }

    if (!rtu_fill_to(p, p->expected, in, len))
        return CSMB_PR_NEED_MORE;

    {
        uint16_t want = csmb_crc16(p->buf, p->expected - 2);
        uint16_t have = (uint16_t)(p->buf[p->expected - 2] |
                                   (p->buf[p->expected - 1] << 8));

        if (want != have)
            return CSMB_PR_BAD;
    }

    p->unit = p->buf[0];
    p->plen = (uint16_t)(p->expected - 3);
    memcpy(p->pdu, p->buf + 1, p->plen);
    return CSMB_PR_FRAME;
}

/* ---- response decode (master) ---- */

int csmb_decode_response(const uint8_t *pdu, size_t len,
                         uint8_t expected_fc, csmb_response *r)
{
    uint8_t fc;

    memset(r, 0, sizeof(*r));
    if (len < 1)
        return CSMB_EINVAL;
    r->fc = pdu[0];
    if ((r->fc & 0x7f) != (expected_fc & 0x7f))
        return CSMB_ERANGE;

    if (r->fc & 0x80) {
        if (len != 2)
            return CSMB_EINVAL;
        r->exception = pdu[1];
        return CSMB_OK;
    }

    fc = r->fc;
    if (fc >= 1 && fc <= 4) {
        if (len < 2)
            return CSMB_EINVAL;
        r->nbytes = pdu[1];
        if (len != (size_t)2 + r->nbytes)
            return CSMB_EINVAL;
        r->data = pdu + 2;
        return CSMB_OK;
    }
    if (fc == 5 || fc == 6 || fc == 15 || fc == 16) {
        if (len != 5)
            return CSMB_EINVAL;
        r->start = rd_u16(pdu + 1);
        r->count_or_value = rd_u16(pdu + 3);
        return CSMB_OK;
    }
    return CSMB_ERANGE;
}

/* ---- request decode (slave) ---- */

int csmb_decode_request(const uint8_t *pdu, size_t len, csmb_request *r)
{
    memset(r, 0, sizeof(*r));
    if (len < 1)
        return CSMB_EINVAL;
    r->fc = pdu[0];

    switch (r->fc) {
    case CSMB_FC_READ_COILS:
    case CSMB_FC_READ_DISCRETE:
    case CSMB_FC_READ_HOLDING:
    case CSMB_FC_READ_INPUT: {
        uint16_t limit;

        if (len != 5)
            return CSMB_EINVAL;
        r->reg_type =
            (r->fc == CSMB_FC_READ_COILS)    ? CSMB_COIL :
            (r->fc == CSMB_FC_READ_DISCRETE) ? CSMB_DISCRETE :
            (r->fc == CSMB_FC_READ_HOLDING)  ? CSMB_HOLDING : CSMB_INPUT;
        r->start = rd_u16(pdu + 1);
        r->count = rd_u16(pdu + 3);
        limit = (r->reg_type == CSMB_COIL || r->reg_type == CSMB_DISCRETE)
                ? CSMB_MAX_READ_FLAGS : CSMB_MAX_READ_WORDS;
        if (r->count < 1 || r->count > limit)
            r->exc = CSMB_EXC_ILLEGAL_VALUE;
        return CSMB_OK;
    }

    case CSMB_FC_WRITE_SINGLE_COIL: {
        uint16_t v;

        if (len != 5)
            return CSMB_EINVAL;
        r->reg_type = CSMB_COIL;
        r->start = rd_u16(pdu + 1);
        r->count = 1;
        v = rd_u16(pdu + 3);
        if (v == 0xFF00)
            r->value = 1;
        else if (v == 0x0000)
            r->value = 0;
        else
            r->exc = CSMB_EXC_ILLEGAL_VALUE;
        return CSMB_OK;
    }

    case CSMB_FC_WRITE_SINGLE_REG:
        if (len != 5)
            return CSMB_EINVAL;
        r->reg_type = CSMB_HOLDING;
        r->start = rd_u16(pdu + 1);
        r->count = 1;
        r->value = rd_u16(pdu + 3);
        return CSMB_OK;

    case CSMB_FC_WRITE_MULTI_COILS:
        if (len < 6)
            return CSMB_EINVAL;
        r->nbytes = pdu[5];
        if (len != (size_t)6 + r->nbytes)
            return CSMB_EINVAL;
        r->reg_type = CSMB_COIL;
        r->start = rd_u16(pdu + 1);
        r->count = rd_u16(pdu + 3);
        if (r->count < 1 || r->count > CSMB_MAX_WRITE_COILS) {
            r->exc = CSMB_EXC_ILLEGAL_VALUE;
            return CSMB_OK;
        }
        if (r->nbytes != (r->count + 7) / 8)
            return CSMB_EINVAL;
        r->data = pdu + 6;
        return CSMB_OK;

    case CSMB_FC_WRITE_MULTI_REGS:
        if (len < 6)
            return CSMB_EINVAL;
        r->nbytes = pdu[5];
        if (len != (size_t)6 + r->nbytes)
            return CSMB_EINVAL;
        r->reg_type = CSMB_HOLDING;
        r->start = rd_u16(pdu + 1);
        r->count = rd_u16(pdu + 3);
        if (r->count < 1 || r->count > CSMB_MAX_WRITE_WORDS) {
            r->exc = CSMB_EXC_ILLEGAL_VALUE;
            return CSMB_OK;
        }
        if (r->nbytes != 2 * r->count)
            return CSMB_EINVAL;
        r->data = pdu + 6;
        return CSMB_OK;

    default:
        r->exc = CSMB_EXC_ILLEGAL_FUNCTION;
        return CSMB_OK;
    }
}

/* ---- PDU builders ---- */

int csmb_build_read(uint8_t *pdu, int reg_type, uint16_t start, uint16_t count)
{
    uint8_t fc;
    uint16_t limit;

    switch (reg_type) {
    case CSMB_COIL:     fc = CSMB_FC_READ_COILS;    limit = CSMB_MAX_READ_FLAGS; break;
    case CSMB_DISCRETE: fc = CSMB_FC_READ_DISCRETE; limit = CSMB_MAX_READ_FLAGS; break;
    case CSMB_HOLDING:  fc = CSMB_FC_READ_HOLDING;  limit = CSMB_MAX_READ_WORDS; break;
    case CSMB_INPUT:    fc = CSMB_FC_READ_INPUT;    limit = CSMB_MAX_READ_WORDS; break;
    default:            return CSMB_EBADTYPE;
    }
    if (count < 1)
        return CSMB_ERANGE;
    if (count > limit)
        return CSMB_ETOOBIG;
    pdu[0] = fc;
    wr_u16(pdu + 1, start);
    wr_u16(pdu + 3, count);
    return 5;
}

int csmb_build_write_single(uint8_t *pdu, int reg_type, uint16_t addr,
                            uint16_t value)
{
    switch (reg_type) {
    case CSMB_COIL:
        pdu[0] = CSMB_FC_WRITE_SINGLE_COIL;
        wr_u16(pdu + 1, addr);
        wr_u16(pdu + 3, value ? 0xFF00 : 0x0000);
        return 5;
    case CSMB_HOLDING:
        pdu[0] = CSMB_FC_WRITE_SINGLE_REG;
        wr_u16(pdu + 1, addr);
        wr_u16(pdu + 3, value);
        return 5;
    default:
        return CSMB_EBADTYPE;
    }
}

int csmb_build_write_multi(uint8_t *pdu, int reg_type, uint16_t start,
                           uint16_t count, const uint16_t *values)
{
    uint16_t i;

    if (reg_type == CSMB_COIL) {
        uint8_t nbytes;

        if (count < 1)
            return CSMB_ERANGE;
        if (count > CSMB_MAX_WRITE_COILS)
            return CSMB_ETOOBIG;
        nbytes = (uint8_t)((count + 7) / 8);
        pdu[0] = CSMB_FC_WRITE_MULTI_COILS;
        wr_u16(pdu + 1, start);
        wr_u16(pdu + 3, count);
        pdu[5] = nbytes;
        memset(pdu + 6, 0, nbytes);
        for (i = 0; i < count; i++)
            if (values[i])
                pdu[6 + (i / 8)] |= (uint8_t)(1u << (i % 8));
        return 6 + nbytes;
    }
    if (reg_type == CSMB_HOLDING) {
        uint8_t nbytes;

        if (count < 1)
            return CSMB_ERANGE;
        if (count > CSMB_MAX_WRITE_WORDS)
            return CSMB_ETOOBIG;
        nbytes = (uint8_t)(2 * count);
        pdu[0] = CSMB_FC_WRITE_MULTI_REGS;
        wr_u16(pdu + 1, start);
        wr_u16(pdu + 3, count);
        pdu[5] = nbytes;
        for (i = 0; i < count; i++)
            wr_u16(pdu + 6 + 2 * i, values[i]);
        return 6 + nbytes;
    }
    return CSMB_EBADTYPE;
}

int csmb_build_exception(uint8_t *pdu, uint8_t fc, uint8_t exc)
{
    pdu[0] = (uint8_t)(fc | 0x80);
    pdu[1] = exc;
    return 2;
}

int csmb_build_read_response(uint8_t *pdu, uint8_t fc, const uint16_t *values,
                             uint16_t count, int coils)
{
    uint16_t i;

    if (coils) {
        uint8_t nbytes;

        if (count < 1 || count > CSMB_MAX_READ_FLAGS)
            return CSMB_ERANGE;
        nbytes = (uint8_t)((count + 7) / 8);
        pdu[0] = fc;
        pdu[1] = nbytes;
        memset(pdu + 2, 0, nbytes);
        for (i = 0; i < count; i++)
            if (values[i])
                pdu[2 + (i / 8)] |= (uint8_t)(1u << (i % 8));
        return 2 + nbytes;
    } else {
        uint8_t nbytes;

        if (count < 1 || count > CSMB_MAX_READ_WORDS)
            return CSMB_ERANGE;
        nbytes = (uint8_t)(2 * count);
        pdu[0] = fc;
        pdu[1] = nbytes;
        for (i = 0; i < count; i++)
            wr_u16(pdu + 2 + 2 * i, values[i]);
        return 2 + nbytes;
    }
}

int csmb_build_write_response(uint8_t *pdu, uint8_t fc, uint16_t start,
                              uint16_t count_or_value)
{
    pdu[0] = fc;
    wr_u16(pdu + 1, start);
    wr_u16(pdu + 3, count_or_value);
    return 5;
}

/* ---- framing wrappers ---- */

int csmb_mbap_wrap(uint8_t *buf, uint16_t tid, uint8_t unit, size_t pdu_len)
{
    if (pdu_len < 1 || pdu_len > CSMB_MAX_PDU)
        return CSMB_ERANGE;
    wr_u16(buf, tid);
    wr_u16(buf + 2, 0);
    wr_u16(buf + 4, (uint16_t)(pdu_len + 1));  /* unit + PDU */
    buf[6] = unit;
    return (int)(CSMB_MBAP_LEN + pdu_len);
}

int csmb_rtu_wrap(uint8_t *buf, uint8_t unit, size_t pdu_len)
{
    uint16_t crc;

    if (pdu_len < 1 || pdu_len > CSMB_MAX_PDU)
        return CSMB_ERANGE;
    buf[0] = unit;
    crc = csmb_crc16(buf, 1 + pdu_len);
    buf[1 + pdu_len] = (uint8_t)(crc & 0xFF);   /* low byte first */
    buf[2 + pdu_len] = (uint8_t)(crc >> 8);
    return (int)(pdu_len + 3);
}
