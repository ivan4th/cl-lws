/* libFuzzer harness: client messages on an established (3.8, None)
 * connection, fed one byte at a time (maximal fragmentation). */

#include "../csvnc-private.h"
#include "fuzz-main.h"

static size_t sent;

static void op_send(void *u, const uint8_t *d, size_t n)
{
    (void)u;
    (void)d;
    sent += n;
}

static void op_void(void *u)
{
    (void)u;
}

static void op_fbur(void *u, int i, int x, int y, int w, int h)
{
    (void)u; (void)i; (void)x; (void)y; (void)w; (void)h;
}

static void op_key(void *u, uint32_t k, int d)
{
    (void)u;
    (void)d;
    csvnc_keysym_to_sdl(k);
}

static void op_ptr(void *u, int x, int y, unsigned b)
{
    (void)u; (void)x; (void)y; (void)b;
}

static const csvnc_rfb_ops ops = {
    op_send, op_void, op_void, op_fbur, op_key, op_ptr
};

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static const uint8_t hello[] = "RFB 003.008\n\x01\x01";
    csvnc_rfb c;
    size_t i;

    csvnc_rfb_init(&c, &ops, NULL, 640, 480, "fuzz");
    csvnc_rfb_start(&c);
    if (csvnc_rfb_feed(&c, hello, 14) < 0 || !csvnc_rfb_established(&c))
        abort();
    for (i = 0; i < size; i++)
        if (csvnc_rfb_feed(&c, data + i, 1) < 0)
            break;
    /* messages never produce output after the handshake */
    if (sent != 12 + 2 + 4 + 24 + 4)
        abort();
    sent = 0;
    return 0;
}
