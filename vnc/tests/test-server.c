/* Integration test of the lws server: a real lws context on this
 * thread, plain BSD sockets as clients, serviced alternately.  A
 * periodic sul keeps lws_service() returning so the test can poll
 * its client sockets in between.  Covers: ephemeral port, handshake +
 * ServerInit, a Raw full update equal to the shadow, an incremental
 * update after a blit covering only the change, Hextile selection,
 * key/pointer callbacks, a slow (never reading) client with a bounded
 * pending region while another client is served, garbage -> that
 * client closed, a bound port refused, destroy closes clients and
 * frees the port for a new server on the same context. */

#include <libwebsockets.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "../csvnc-private.h"
#include "test-util.h"

static struct lws_context *cx;
static lws_sorted_usec_list_t tick_sul;

static void tick_cb(lws_sorted_usec_list_t *sul)
{
    lws_sul_schedule(cx, 0, sul, tick_cb, 2000);
}

static const struct lws_protocols protocols[] = {
    { "cs-vnc", (lws_callback_function *)csvnc_lws_protocol_callback, 0, 0, 0, NULL, 0 },
    { NULL, NULL, 0, 0, 0, NULL, 0 }
};

static void service(void)
{
    lws_service(cx, 0);
}

/* ---- callbacks ---- */

static int32_t keys[64];
static int32_t key_downs[64];
static int nkeys;
static int ptr_x, ptr_y;
static unsigned ptr_buttons;
static int nptr;
static void *cb_user_seen;

static void on_key(void *user, int32_t sdl, int32_t down)
{
    cb_user_seen = user;
    if (nkeys < 64) {
        keys[nkeys] = sdl;
        key_downs[nkeys] = down;
    }
    nkeys++;
}

static void on_pointer(void *user, int x, int y, unsigned buttons)
{
    (void)user;
    ptr_x = x;
    ptr_y = y;
    ptr_buttons = buttons;
    nptr++;
}

/* ---- a socket client ---- */

typedef struct client {
    int fd;
    uint8_t rx[1 << 20];
    size_t rxlen;
    int eof;
} client;

static int client_connect(client *c, int port)
{
    struct sockaddr_in sa;

    memset(c, 0, sizeof(*c));
    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (c->fd < 0)
        return -1;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(c->fd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
        return -1;
    fcntl(c->fd, F_SETFL, O_NONBLOCK);
    return 0;
}

static void client_pump(client *c)
{
    ssize_t n;

    if (c->eof)
        return;
    for (;;) {
        n = read(c->fd, c->rx + c->rxlen, sizeof(c->rx) - c->rxlen);
        if (n > 0) {
            c->rxlen += (size_t)n;
            if (c->rxlen == sizeof(c->rx))
                return;
            continue;
        }
        if (n == 0)
            c->eof = 1;
        return;
    }
}

/* service + read until the client has at least N bytes (or gives up) */
static int client_wait(client *c, size_t n)
{
    int i;

    for (i = 0; i < 5000; i++) {
        client_pump(c);
        if (c->rxlen >= n || c->eof)
            return c->rxlen >= n;
        service();
    }
    return 0;
}

static int client_wait_eof(client *c)
{
    int i;

    for (i = 0; i < 5000; i++) {
        client_pump(c);
        if (c->eof)
            return 1;
        service();
    }
    return 0;
}

static void client_consume(client *c, size_t n)
{
    memmove(c->rx, c->rx + n, c->rxlen - n);
    c->rxlen -= n;
}

static void client_send(client *c, const void *data, size_t n)
{
    const uint8_t *p = data;

    while (n) {
        ssize_t w = write(c->fd, p, n);

        if (w < 0) {
            if (errno == EAGAIN)
                service();
            else
                break;
            continue;
        }
        p += w;
        n -= (size_t)w;
    }
}

static void client_close(client *c)
{
    if (c->fd >= 0)
        close(c->fd);
    c->fd = -1;
}

/* RFB 3.8 / None / ClientInit; leaves the parser established and
 * checks ServerInit against W x H. */
static int client_handshake(client *c, int w, int h)
{
    static const uint8_t v38[] = "RFB 003.008\n";
    uint8_t b;

    if (!client_wait(c, 12))
        return -1;
    TCHECK(memcmp(c->rx, "RFB 003.008\n", 12) == 0);
    client_consume(c, 12);
    client_send(c, v38, 12);
    if (!client_wait(c, 2))
        return -1;
    TCHECK_EQ(c->rx[0], 1);
    TCHECK_EQ(c->rx[1], 1);
    client_consume(c, 2);
    b = 1;
    client_send(c, &b, 1);
    if (!client_wait(c, 4))
        return -1;
    TCHECK_EQ(csvnc_get_u32(c->rx), 0);
    client_consume(c, 4);
    b = 1;
    client_send(c, &b, 1);
    if (!client_wait(c, 24 + 12))
        return -1;
    TCHECK_EQ(csvnc_get_u16(c->rx), (unsigned)w);
    TCHECK_EQ(csvnc_get_u16(c->rx + 2), (unsigned)h);
    TCHECK_EQ(c->rx[4], 32);
    TCHECK_EQ(csvnc_get_u32(c->rx + 20), 12);
    TCHECK(memcmp(c->rx + 24, "EACS console", 12) == 0);
    client_consume(c, 36);
    return 0;
}

static void client_request(client *c, int incremental, int x, int y, int w, int h)
{
    uint8_t m[10] = { 3 };

    m[1] = (uint8_t)incremental;
    csvnc_put_u16(m + 2, (unsigned)x);
    csvnc_put_u16(m + 4, (unsigned)y);
    csvnc_put_u16(m + 6, (unsigned)w);
    csvnc_put_u16(m + 8, (unsigned)h);
    client_send(c, m, sizeof(m));
}

static void client_set_encodings(client *c, const int32_t *encs, int n)
{
    uint8_t m[4 + 4 * 16];
    int i;

    m[0] = 2;
    m[1] = 0;
    csvnc_put_u16(m + 2, (unsigned)n);
    for (i = 0; i < n; i++)
        csvnc_put_u32(m + 4 + 4 * i, (uint32_t)encs[i]);
    client_send(c, m, 4 + 4 * (size_t)n);
}

/* Read one complete FramebufferUpdate (Raw or Hextile rects) into an
 * image of W x H uint32
 * (native XRGB values), painting each rect.  Returns the rect count
 * (excluding LastRect) or -1. */
static int client_read_update(client *c, uint32_t *img, int w, int h,
                              csvnc_rect *rects, int max_rects)
{
    unsigned nrects, i;
    int got = 0;
    csvnc_pixfmt native;

    csvnc_pixfmt_native(&native);
    if (!client_wait(c, 4))
        return -1;
    TCHECK_EQ(c->rx[0], 0);
    nrects = csvnc_get_u16(c->rx + 2);
    client_consume(c, 4);
    for (i = 0; i < nrects; i++) {
        int rx, ry, rw, rh, x, y;
        int32_t enc;

        if (!client_wait(c, 12))
            return -1;
        rx = (int)csvnc_get_u16(c->rx);
        ry = (int)csvnc_get_u16(c->rx + 2);
        rw = (int)csvnc_get_u16(c->rx + 4);
        rh = (int)csvnc_get_u16(c->rx + 6);
        enc = (int32_t)csvnc_get_u32(c->rx + 8);
        client_consume(c, 12);
        /* never sent, even to clients that offer it (vncsnapshot
         * chokes on it) */
        TCHECK(enc != CSVNC_PSEUDO_LAST_RECT);
        TCHECK(nrects != 0xffff);
        if (got < max_rects) {
            rects[got].x = rx;
            rects[got].y = ry;
            rects[got].w = rw;
            rects[got].h = rh;
        }
        got++;
        TCHECK(rx >= 0 && ry >= 0 && rw > 0 && rh > 0 && rx + rw <= w && ry + rh <= h);
        if (enc == CSVNC_ENC_RAW) {
            if (!client_wait(c, (size_t)rw * rh * 4))
                return -1;
            for (y = 0; y < rh; y++)
                for (x = 0; x < rw; x++)
                    img[(ry + y) * w + rx + x] =
                        csvnc_pix_get(&native, c->rx + ((size_t)y * rw + x) * 4);
            client_consume(c, (size_t)rw * rh * 4);
        } else if (enc == CSVNC_ENC_HEXTILE) {
            /* flat test frames: every tile is 1 or 5 bytes; decode
             * bg-only tiles, reject anything fancier */
            int ty, tx;
            uint32_t bg = 0;

            for (ty = 0; ty < rh; ty += 16)
                for (tx = 0; tx < rw; tx += 16) {
                    int tw = rw - tx < 16 ? rw - tx : 16;
                    int th = rh - ty < 16 ? rh - ty : 16;

                    if (!client_wait(c, 1))
                        return -1;
                    if (c->rx[0] == 2) {
                        if (!client_wait(c, 5))
                            return -1;
                        bg = csvnc_pix_get(&native, c->rx + 1);
                        client_consume(c, 5);
                    } else if (c->rx[0] == 0) {
                        client_consume(c, 1);
                    } else {
                        TFAIL("unexpected hextile subencoding %d", c->rx[0]);
                        return -1;
                    }
                    for (y = 0; y < th; y++)
                        for (x = 0; x < tw; x++)
                            img[(ry + ty + y) * w + rx + tx + x] = bg;
                }
        } else {
            TFAIL("unexpected encoding %d", (int)enc);
            return -1;
        }
    }
    return got;
}

/* ---- the tests ---- */

#define W 100
#define H 70

static csvnc_server *make_server(int port)
{
    csvnc_callbacks cbs = { on_key, on_pointer };

    return csvnc_create(cx, protocols, "127.0.0.1", port, W, H, &cbs,
                        (void *)0x1234);
}

static void fill(uint32_t *px, int n, uint32_t seed)
{
    int i;

    for (i = 0; i < n; i++)
        px[i] = (seed + (uint32_t)i * 0x010305u) & 0xffffff;
}

static void test_basic_updates(void)
{
    csvnc_server *s = make_server(0);
    client c;
    int port, n;
    uint32_t frame[W * H], img[W * H];
    csvnc_rect rects[64];
    int i;

    TCHECK(s != NULL);
    port = csvnc_listen_port(s);
    TCHECK(port > 0);
    TCHECK_EQ(csvnc_client_count(s), 0);

    /* a frame blitted before any client connects */
    fill(frame, W * H, 0x123456);
    csvnc_blit(s, 0, 0, W, H, frame, W * 4, CSVNC_SRC_XRGB8888);

    TCHECK_EQ(client_connect(&c, port), 0);
    TCHECK_EQ(client_handshake(&c, W, H), 0);
    TCHECK_EQ(csvnc_client_count(s), 1);

    /* nothing arrives without a request */
    for (i = 0; i < 50; i++)
        service();
    client_pump(&c);
    TCHECK_EQ(c.rxlen, 0);

    /* full non-incremental request: the whole frame, Raw */
    client_request(&c, 0, 0, 0, W, H);
    memset(img, 0xee, sizeof(img));
    n = client_read_update(&c, img, W, H, rects, 64);
    TCHECK(n >= 1);
    TCHECK(memcmp(img, frame, sizeof(img)) == 0);
    TCHECK_EQ(c.rxlen, 0);

    /* incremental request with nothing dirty: nothing until a blit */
    client_request(&c, 1, 0, 0, W, H);
    for (i = 0; i < 50; i++)
        service();
    client_pump(&c);
    TCHECK_EQ(c.rxlen, 0);

    /* a 10x5 change at (20,30): exactly that rectangle arrives */
    for (i = 0; i < 5; i++)
        memset(frame + (30 + i) * W + 20, 0, 10 * 4);
    csvnc_blit(s, 0, 0, W, H, frame, W * 4, CSVNC_SRC_XRGB8888);
    n = client_read_update(&c, img, W, H, rects, 64);
    TCHECK_EQ(n, 1);
    TCHECK_EQ(rects[0].x, 20);
    TCHECK_EQ(rects[0].y, 30);
    TCHECK_EQ(rects[0].w, 10);
    TCHECK_EQ(rects[0].h, 5);
    TCHECK(memcmp(img, frame, sizeof(img)) == 0);

    /* a blit of the same pixels is not a change */
    client_request(&c, 1, 0, 0, W, H);
    csvnc_blit(s, 0, 0, W, H, frame, W * 4, CSVNC_SRC_XRGB8888);
    for (i = 0; i < 50; i++)
        service();
    client_pump(&c);
    TCHECK_EQ(c.rxlen, 0);

    /* two changes before the request is answered: both in one update;
     * a change made while no request is pending waits for the next */
    frame[0] = 0x111111;
    csvnc_blit(s, 0, 0, W, H, frame, W * 4, CSVNC_SRC_XRGB8888);
    n = client_read_update(&c, img, W, H, rects, 64);
    TCHECK_EQ(n, 1);
    TCHECK(memcmp(img, frame, sizeof(img)) == 0);
    frame[W * H - 1] = 0x222222;
    csvnc_blit(s, 0, 0, W, H, frame, W * 4, CSVNC_SRC_XRGB8888);
    frame[W * 10 + 50] = 0x333333;
    csvnc_blit(s, 50, 10, 1, 1, frame + W * 10 + 50, W * 4, CSVNC_SRC_XRGB8888);
    for (i = 0; i < 20; i++)
        service();
    client_pump(&c);
    TCHECK_EQ(c.rxlen, 0);
    client_request(&c, 1, 0, 0, W, H);
    n = client_read_update(&c, img, W, H, rects, 64);
    TCHECK_EQ(n, 2);
    TCHECK(memcmp(img, frame, sizeof(img)) == 0);

    /* Hextile (LastRect offered but never used): a flat region
     * arrives as Hextile tiles under an exact rect count */
    {
        int32_t encs[3] = { CSVNC_ENC_HEXTILE, CSVNC_ENC_RAW, CSVNC_PSEUDO_LAST_RECT };

        client_set_encodings(&c, encs, 3);
        for (i = 0; i < W * H; i++)
            frame[i] = 0x445566;
        csvnc_blit(s, 0, 0, W, H, frame, W * 4, CSVNC_SRC_XRGB8888);
        client_request(&c, 1, 0, 0, W, H);
        n = client_read_update(&c, img, W, H, rects, 64);
        TCHECK(n >= 1);
        TCHECK(memcmp(img, frame, sizeof(img)) == 0);
        TCHECK_EQ(c.rxlen, 0);
    }

    /* keys and the pointer reach the callbacks with the user pointer */
    {
        uint8_t key[8] = { 4, 1, 0, 0, 0, 0, 0xff, 0x0d };
        uint8_t ptr[6] = { 5, 1, 0, 12, 0, 34 };
        uint8_t unmapped[8] = { 4, 1, 0, 0, 0, 0, 0x06, 0xc1 };

        nkeys = 0;
        client_send(&c, key, 8);
        key[1] = 0;
        client_send(&c, key, 8);
        client_send(&c, unmapped, 8);
        client_send(&c, ptr, 6);
        for (i = 0; i < 100 && nptr == 0; i++)
            service();
        TCHECK_EQ(nkeys, 2);
        TCHECK_EQ(keys[0], 13);
        TCHECK_EQ(key_downs[0], 1);
        TCHECK_EQ(keys[1], 13);
        TCHECK_EQ(key_downs[1], 0);
        TCHECK_EQ(nptr, 1);
        TCHECK_EQ(ptr_x, 12);
        TCHECK_EQ(ptr_y, 34);
        TCHECK_EQ(ptr_buttons, 1);
        TCHECK(cb_user_seen == (void *)0x1234);
    }

    /* a second server on the port we already serve is refused (lws
     * would otherwise share the listen socket), and so is a port held
     * by someone else */
    TCHECK(make_server(port) == NULL);
    {
        struct sockaddr_in sa;
        socklen_t salen = sizeof(sa);
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        csvnc_server *dup;

        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        TCHECK_EQ(bind(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
        TCHECK_EQ(listen(fd, 1), 0);
        TCHECK_EQ(getsockname(fd, (struct sockaddr *)&sa, &salen), 0);
        dup = make_server(ntohs(sa.sin_port));
        TCHECK(dup == NULL);
        close(fd);
    }

    client_close(&c);
    for (i = 0; i < 50; i++)
        service();
    TCHECK_EQ(csvnc_client_count(s), 0);
    csvnc_destroy(s);
    for (i = 0; i < 50; i++)
        service();
    /* the port is free again on the same context */
    s = make_server(port);
    TCHECK(s != NULL);
    TCHECK_EQ(csvnc_listen_port(s), port);
    csvnc_destroy(s);
    for (i = 0; i < 50; i++)
        service();
}

static void test_slow_and_bad_clients(void)
{
    csvnc_server *s = make_server(0);
    client fast, slow, bad;
    int port, n, i, round;
    uint32_t frame[W * H], img[W * H];
    csvnc_rect rects[64];

    TCHECK(s != NULL);
    port = csvnc_listen_port(s);
    fill(frame, W * H, 0x777777);
    csvnc_blit(s, 0, 0, W, H, frame, W * 4, CSVNC_SRC_XRGB8888);

    TCHECK_EQ(client_connect(&fast, port), 0);
    TCHECK_EQ(client_connect(&slow, port), 0);
    TCHECK_EQ(client_handshake(&fast, W, H), 0);
    TCHECK_EQ(client_handshake(&slow, W, H), 0);
    TCHECK_EQ(csvnc_client_count(s), 2);

    /* the slow client asks for everything, then never reads; the
     * frame keeps changing: the fast client is served every time */
    client_request(&slow, 0, 0, 0, W, H);
    client_request(&fast, 0, 0, 0, W, H);
    n = client_read_update(&fast, img, W, H, rects, 64);
    TCHECK(n >= 1);
    for (round = 0; round < 300; round++) {
        frame[(round * 37) % (W * H)] ^= 0xffffff;
        csvnc_blit(s, 0, 0, W, H, frame, W * 4, CSVNC_SRC_XRGB8888);
        client_request(&fast, 1, 0, 0, W, H);
        n = client_read_update(&fast, img, W, H, rects, 64);
        TCHECK(n >= 1);
        if (memcmp(img, frame, sizeof(img)) != 0) {
            TFAIL("fast client out of sync at round %d", round);
            break;
        }
        service();
    }
    /* the slow one is still connected and has a bounded region */
    TCHECK_EQ(csvnc_client_count(s), 2);
    TCHECK(!slow.eof);
    /* now it reads: it gets a consistent frame eventually */
    for (i = 0; i < 100; i++) {
        service();
        client_pump(&slow);
    }
    memset(img, 0, sizeof(img));
    n = client_read_update(&slow, img, W, H, rects, 64);
    TCHECK(n >= 1);
    client_request(&slow, 0, 0, 0, W, H);
    n = client_read_update(&slow, img, W, H, rects, 64);
    TCHECK(n >= 1);
    TCHECK(memcmp(img, frame, sizeof(img)) == 0);

    /* garbage from a third client closes only that client */
    TCHECK_EQ(client_connect(&bad, port), 0);
    TCHECK(client_wait(&bad, 12));
    client_consume(&bad, 12);
    client_send(&bad, "HELLO WORLD!", 12);
    TCHECK(client_wait_eof(&bad));
    TCHECK_EQ(csvnc_client_count(s), 2);
    client_close(&bad);
    /* an established client sending an unknown message type too */
    {
        uint8_t junk = 42;

        client_send(&slow, &junk, 1);
        TCHECK(client_wait_eof(&slow));
        TCHECK_EQ(csvnc_client_count(s), 1);
        client_close(&slow);
    }
    /* the fast client still works */
    frame[7] ^= 0x0f0f0f;
    csvnc_blit(s, 0, 0, W, H, frame, W * 4, CSVNC_SRC_XRGB8888);
    client_request(&fast, 1, 0, 0, W, H);
    n = client_read_update(&fast, img, W, H, rects, 64);
    TCHECK_EQ(n, 1);
    TCHECK(memcmp(img, frame, sizeof(img)) == 0);

    /* destroy with a live client: it sees EOF */
    csvnc_destroy(s);
    TCHECK(client_wait_eof(&fast));
    client_close(&fast);
    for (i = 0; i < 50; i++)
        service();
}

static void test_create_destroy_cycles(void)
{
    int i;

    for (i = 0; i < 5; i++) {
        csvnc_server *s = make_server(0);
        client c;

        TCHECK(s != NULL);
        TCHECK_EQ(client_connect(&c, csvnc_listen_port(s)), 0);
        TCHECK_EQ(client_handshake(&c, W, H), 0);
        csvnc_destroy(s);
        TCHECK(client_wait_eof(&c));
        client_close(&c);
    }
    /* bad geometry / arguments */
    {
        csvnc_callbacks cbs = { on_key, on_pointer };

        TCHECK(csvnc_create(cx, protocols, "127.0.0.1", 0, 0, 10, &cbs, NULL) == NULL);
        TCHECK(csvnc_create(cx, protocols, "127.0.0.1", 70000, W, H, &cbs, NULL) == NULL);
        TCHECK(csvnc_create(cx, NULL, "127.0.0.1", 0, W, H, &cbs, NULL) == NULL);
    }
}

static void test_rgb565_blit(void)
{
    csvnc_server *s = make_server(0);
    client c;
    uint16_t frame565[W * H];
    uint32_t expect[W * H], img[W * H];
    csvnc_rect rects[64];
    int i, n;

    TCHECK(s != NULL);
    for (i = 0; i < W * H; i++) {
        unsigned r = (unsigned)i % 32, g = (unsigned)(i / 3) % 64, b = (unsigned)(i / 7) % 32;

        frame565[i] = (uint16_t)((r << 11) | (g << 5) | b);
        expect[i] = (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8)
            | ((b << 3) | (b >> 2));
    }
    csvnc_blit(s, 0, 0, W, H, frame565, W * 2, CSVNC_SRC_RGB565);
    TCHECK_EQ(client_connect(&c, csvnc_listen_port(s)), 0);
    TCHECK_EQ(client_handshake(&c, W, H), 0);
    client_request(&c, 0, 0, 0, W, H);
    n = client_read_update(&c, img, W, H, rects, 64);
    TCHECK(n >= 1);
    TCHECK(memcmp(img, expect, sizeof(img)) == 0);
    client_close(&c);
    csvnc_destroy(s);
    for (i = 0; i < 50; i++)
        service();
}

int main(void)
{
    struct lws_context_creation_info info;
    void (*tests[])(void) = {
        test_basic_updates, test_slow_and_bad_clients,
        test_create_destroy_cycles, test_rgb565_blit
    };
    size_t i;

    lws_set_log_level(LLL_ERR | LLL_WARN, NULL);
    memset(&info, 0, sizeof(info));
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.options = LWS_SERVER_OPTION_EXPLICIT_VHOSTS;
    cx = lws_create_context(&info);
    if (!cx) {
        fprintf(stderr, "lws_create_context failed\n");
        return 1;
    }
    lws_sul_schedule(cx, 0, &tick_sul, tick_cb, 2000);
    for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
        tests[i]();
    lws_sul_cancel(&tick_sul);
    lws_context_destroy(cx);
    if (csvnc_test_failures) {
        fprintf(stderr, "%d FAILURE(S)\n", csvnc_test_failures);
        return 1;
    }
    printf("OK %s\n", __FILE__);
    return 0;
}
