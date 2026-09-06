/* libFuzzer harness: pixel-format parsing plus Raw/Hextile encoding of
 * a small data-derived frame into a buffer sized by the encoder's own
 * bound (ASan catches any overrun of it). */

#include "../csvnc-private.h"
#include "fuzz-main.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    csvnc_pixfmt f;
    csvnc_fb fb;
    csvnc_encoder e;
    int w, h, x, y, rw, rh, i, npx;
    size_t cap, n;
    uint8_t *buf;

    if (size < 22)
        return 0;
    if (csvnc_pixfmt_parse(&f, data) < 0)
        csvnc_pixfmt_native(&f);
    w = (data[16] % 40) + 1;
    h = (data[17] % 40) + 1;
    x = data[18] % w;
    y = data[19] % h;
    rw = data[20] % (w - x) + 1;
    rh = data[21] % (h - y) + 1;
    data += 22;
    size -= 22;
    if (csvnc_fb_init(&fb, w, h) < 0)
        abort();
    npx = w * h;
    /* the frame is the remaining data, repeated, as 0x00RRGGBB with a
     * data-driven flat run at the start so hextile sees both kinds */
    for (i = 0; i < npx; i++) {
        size_t k = size ? (size_t)i * 3 % size : 0;
        uint32_t v = size ? ((uint32_t)data[k] << 16)
            | ((uint32_t)data[(k + 1) % size] << 8) | data[(k + 2) % size] : 0;

        fb.px[i] = (size && i < (int)(data[0] % (npx + 1))) ? 0x123456 : v;
    }
    csvnc_encoder_init(&e);
    csvnc_encoder_select(&e, !size ? CSVNC_ENC_RAW
                         : (data[0] & 3) == 1 ? CSVNC_ENC_HEXTILE
                         : (data[0] & 3) == 2 ? CSVNC_ENC_ZRLE : CSVNC_ENC_RAW);
    cap = csvnc_encode_max_size(&e, &f, rw, rh);
    buf = malloc(cap);
    if (!buf)
        abort();
    n = csvnc_encode_rect(&e, &f, &fb, x, y, rw, rh, buf, cap);
    if (n == 0 || n > cap)
        abort();
    /* a too-small buffer must be refused, never overrun.  Raw and
     * Hextile are deterministic, so one byte less than N must fail; a
     * fresh ZRLE stream may legitimately produce fewer bytes than the
     * continuing one did, so it is only asked to fit in 3 bytes (less
     * than its length prefix).  A refused ZRLE rect desynchronises
     * its stream by design, hence the fresh encoder. */
    {
        csvnc_encoder e2;
        size_t small = e.enc == CSVNC_ENC_ZRLE ? CSVNC_RECT_HDR_SIZE + 3 : n - 1;

        csvnc_encoder_init(&e2);
        csvnc_encoder_select(&e2, e.enc);
        if (n > 1 && csvnc_encode_rect(&e2, &f, &fb, x, y, rw, rh, buf, small) != 0)
            abort();
        csvnc_encoder_free(&e2);
    }
    free(buf);
    csvnc_encoder_free(&e);
    csvnc_fb_free(&fb);
    return 0;
}
