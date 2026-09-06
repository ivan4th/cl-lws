/* RFB handshake and client message parser.
 *
 * A pull-free push parser: the caller feeds whatever bytes arrived;
 * fixed-size message heads are assembled in a 20-byte buffer, the
 * variable-length tails (SetEncodings list, ClientCutText) are consumed
 * as they stream by, so memory per connection is constant whatever the
 * client claims as a length.  Handshake replies are emitted through
 * ops->send as soon as the corresponding client bytes are in. */

#include "csvnc-private.h"

#define RFB_VERSION_LEN 12
#define RFB_SECURITY_NONE 1

static void fail(csvnc_rfb *c)
{
    c->state = CSVNC_ST_FAILED;
}

static void expect(csvnc_rfb *c, int state, unsigned need)
{
    c->state = state;
    c->have = 0;
    c->need = need;
}

static void send_bytes(csvnc_rfb *c, const uint8_t *data, size_t len)
{
    c->ops->send(c->user, data, len);
}

static void send_u32(csvnc_rfb *c, uint32_t v)
{
    uint8_t b[4];

    csvnc_put_u32(b, v);
    send_bytes(c, b, 4);
}

void csvnc_rfb_init(csvnc_rfb *c, const csvnc_rfb_ops *ops, void *user,
                    int width, int height, const char *name)
{
    memset(c, 0, sizeof(*c));
    c->ops = ops;
    c->user = user;
    c->width = width;
    c->height = height;
    c->name = name ? name : "";
    csvnc_pixfmt_native(&c->fmt);
    c->enc = CSVNC_ENC_RAW;
    expect(c, CSVNC_ST_VERSION, RFB_VERSION_LEN);
}

void csvnc_rfb_start(csvnc_rfb *c)
{
    send_bytes(c, (const uint8_t *)"RFB 003.008\n", RFB_VERSION_LEN);
}

static void send_server_init(csvnc_rfb *c)
{
    uint8_t hdr[2 + 2 + 16 + 4];
    size_t namelen = strlen(c->name);

    csvnc_put_u16(hdr, (unsigned)c->width);
    csvnc_put_u16(hdr + 2, (unsigned)c->height);
    csvnc_pixfmt_write(&c->fmt, hdr + 4);
    csvnc_put_u32(hdr + 20, (uint32_t)namelen);
    send_bytes(c, hdr, sizeof(hdr));
    if (namelen)
        send_bytes(c, (const uint8_t *)c->name, namelen);
}

static int parse_version(const uint8_t *b, int *minor)
{
    int i;

    if (memcmp(b, "RFB ", 4) != 0 || b[7] != '.' || b[11] != '\n')
        return -1;
    for (i = 4; i < 11; i++)
        if (i != 7 && (b[i] < '0' || b[i] > '9'))
            return -1;
    if (memcmp(b + 4, "003", 3) != 0)
        return -1;
    *minor = (b[8] - '0') * 100 + (b[9] - '0') * 10 + (b[10] - '0');
    return 0;
}

static void on_version(csvnc_rfb *c)
{
    int minor;

    if (parse_version(c->buf, &minor) < 0) {
        fail(c);
        return;
    }
    /* per the spec anything other than 3.7/3.8 is treated as 3.3 */
    if (minor >= 8)
        c->version = 38;
    else if (minor == 7)
        c->version = 37;
    else
        c->version = 33;

    if (c->version == 33) {
        send_u32(c, RFB_SECURITY_NONE);
        expect(c, CSVNC_ST_CLIENT_INIT, 1);
    } else {
        static const uint8_t types[2] = { 1, RFB_SECURITY_NONE };

        send_bytes(c, types, sizeof(types));
        expect(c, CSVNC_ST_SECURITY_TYPE, 1);
    }
}

static void on_security_type(csvnc_rfb *c)
{
    if (c->buf[0] != RFB_SECURITY_NONE) {
        if (c->version == 38) {
            static const char reason[] = "unsupported security type";

            send_u32(c, 1);
            send_u32(c, sizeof(reason) - 1);
            send_bytes(c, (const uint8_t *)reason, sizeof(reason) - 1);
        }
        fail(c);
        return;
    }
    if (c->version == 38)
        send_u32(c, 0);                 /* SecurityResult OK */
    expect(c, CSVNC_ST_CLIENT_INIT, 1);
}

static void on_client_init(csvnc_rfb *c)
{
    /* the shared flag is ignored: the server is always shared */
    send_server_init(c);
    expect(c, CSVNC_ST_MSG_TYPE, 1);
}

static void on_msg_type(csvnc_rfb *c)
{
    c->msg = c->buf[0];
    switch (c->msg) {
    case CSVNC_MSG_SET_PIXEL_FORMAT:
        expect(c, CSVNC_ST_MSG_BODY, 3 + 16);
        break;
    case CSVNC_MSG_SET_ENCODINGS:
        expect(c, CSVNC_ST_MSG_BODY, 1 + 2);
        break;
    case CSVNC_MSG_FB_UPDATE_REQUEST:
        expect(c, CSVNC_ST_MSG_BODY, 1 + 8);
        break;
    case CSVNC_MSG_KEY_EVENT:
        expect(c, CSVNC_ST_MSG_BODY, 1 + 2 + 4);
        break;
    case CSVNC_MSG_POINTER_EVENT:
        expect(c, CSVNC_ST_MSG_BODY, 1 + 4);
        break;
    case CSVNC_MSG_CLIENT_CUT_TEXT:
        expect(c, CSVNC_ST_MSG_BODY, 3 + 4);
        break;
    default:
        fail(c);
        break;
    }
}

static void consider_encoding(csvnc_rfb *c, int32_t enc)
{
    switch (enc) {
    case CSVNC_ENC_RAW:
    case CSVNC_ENC_HEXTILE:
    case CSVNC_ENC_ZRLE:
        /* the client lists encodings in preference order: keep the
         * first one we support */
        if (!c->enc_chosen) {
            c->enc = enc;
            c->enc_chosen = 1;
        }
        break;
    case CSVNC_PSEUDO_LAST_RECT:
        c->lastrect = 1;
        break;
    default:
        break;
    }
}

static void on_msg_body(csvnc_rfb *c)
{
    const uint8_t *b = c->buf;

    switch (c->msg) {
    case CSVNC_MSG_SET_PIXEL_FORMAT:
        if (csvnc_pixfmt_parse(&c->fmt, b + 3) < 0) {
            fail(c);
            return;
        }
        if (c->ops->pixel_format)
            c->ops->pixel_format(c->user);
        expect(c, CSVNC_ST_MSG_TYPE, 1);
        break;
    case CSVNC_MSG_SET_ENCODINGS: {
        unsigned n = csvnc_get_u16(b + 1);

        c->enc = CSVNC_ENC_RAW;
        c->enc_chosen = 0;
        c->lastrect = 0;
        c->remaining = n * 4;
        if (n == 0) {
            if (c->ops->encodings)
                c->ops->encodings(c->user);
            expect(c, CSVNC_ST_MSG_TYPE, 1);
        } else {
            expect(c, CSVNC_ST_ENCODINGS, 4);
        }
        break;
    }
    case CSVNC_MSG_FB_UPDATE_REQUEST:
        if (c->ops->update_request)
            c->ops->update_request(c->user, b[0] != 0,
                                   (int)csvnc_get_u16(b + 1),
                                   (int)csvnc_get_u16(b + 3),
                                   (int)csvnc_get_u16(b + 5),
                                   (int)csvnc_get_u16(b + 7));
        expect(c, CSVNC_ST_MSG_TYPE, 1);
        break;
    case CSVNC_MSG_KEY_EVENT:
        if (c->ops->key)
            c->ops->key(c->user, csvnc_get_u32(b + 3), b[0] != 0);
        expect(c, CSVNC_ST_MSG_TYPE, 1);
        break;
    case CSVNC_MSG_POINTER_EVENT:
        if (c->ops->pointer)
            c->ops->pointer(c->user, (int)csvnc_get_u16(b + 1),
                            (int)csvnc_get_u16(b + 3), b[0]);
        expect(c, CSVNC_ST_MSG_TYPE, 1);
        break;
    case CSVNC_MSG_CLIENT_CUT_TEXT: {
        uint32_t len = csvnc_get_u32(b + 3);

        /* bit 31 set = extended clipboard, which we never advertise */
        if (len > CSVNC_MAX_CUT_TEXT) {
            fail(c);
            return;
        }
        c->remaining = len;
        if (len == 0)
            expect(c, CSVNC_ST_MSG_TYPE, 1);
        else
            c->state = CSVNC_ST_CUT_TEXT;
        break;
    }
    default:
        fail(c);
        break;
    }
}

static void on_encoding(csvnc_rfb *c)
{
    consider_encoding(c, (int32_t)csvnc_get_u32(c->buf));
    c->remaining -= 4;
    if (c->remaining == 0) {
        if (c->ops->encodings)
            c->ops->encodings(c->user);
        expect(c, CSVNC_ST_MSG_TYPE, 1);
    } else {
        expect(c, CSVNC_ST_ENCODINGS, 4);
    }
}

int csvnc_rfb_feed(csvnc_rfb *c, const uint8_t *data, size_t len)
{
    while (len > 0 && c->state != CSVNC_ST_FAILED) {
        size_t take;

        if (c->state == CSVNC_ST_CUT_TEXT) {
            take = len < c->remaining ? len : c->remaining;
            data += take;
            len -= take;
            c->remaining -= (uint32_t)take;
            if (c->remaining == 0)
                expect(c, CSVNC_ST_MSG_TYPE, 1);
            continue;
        }

        take = c->need - c->have;
        if (take > len)
            take = len;
        memcpy(c->buf + c->have, data, take);
        c->have += (unsigned)take;
        data += take;
        len -= take;
        if (c->have < c->need)
            break;

        switch (c->state) {
        case CSVNC_ST_VERSION:
            on_version(c);
            break;
        case CSVNC_ST_SECURITY_TYPE:
            on_security_type(c);
            break;
        case CSVNC_ST_CLIENT_INIT:
            on_client_init(c);
            break;
        case CSVNC_ST_MSG_TYPE:
            on_msg_type(c);
            break;
        case CSVNC_ST_MSG_BODY:
            on_msg_body(c);
            break;
        case CSVNC_ST_ENCODINGS:
            on_encoding(c);
            break;
        default:
            fail(c);
            break;
        }
    }
    return c->state == CSVNC_ST_FAILED ? -1 : 0;
}
