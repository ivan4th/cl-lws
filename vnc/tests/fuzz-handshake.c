/* libFuzzer harness: the RFB handshake from a fresh connection, input
 * split into chunks of a data-derived size so fragmentation varies. */

#include "../csvnc-private.h"
#include "fuzz-main.h"

static void op_send(void *u, const uint8_t *d, size_t n)
{
    (void)u;
    (void)d;
    (void)n;
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
    static const char *const versions[] = {
        NULL, "RFB 003.008\n", "RFB 003.007\n", "RFB 003.003\n"
    };
    const char *prefix;
    csvnc_rfb c;
    size_t chunk, i = 0;

    if (size == 0)
        return 0;
    chunk = (data[0] & 15) + 1;
    /* bits 4-5 optionally supply a valid ProtocolVersion so the blind
     * standalone driver also exercises the security/init states */
    prefix = versions[(data[0] >> 4) & 3];
    data++;
    size--;
    csvnc_rfb_init(&c, &ops, NULL, 640, 480, "fuzz");
    csvnc_rfb_start(&c);
    if (prefix && csvnc_rfb_feed(&c, (const uint8_t *)prefix, 12) < 0)
        abort();
    while (i < size) {
        size_t n = size - i < chunk ? size - i : chunk;

        if (csvnc_rfb_feed(&c, data + i, n) < 0)
            break;
        i += n;
    }
    return 0;
}
