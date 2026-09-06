/* Shared libFuzzer entry declaration plus a standalone driver.
 *
 * Built with -DCSVNC_LIBFUZZER (clang -fsanitize=fuzzer), libFuzzer
 * provides main() and calls LLVMFuzzerTestOneInput.  Without that define
 * a small standalone main() feeds pseudo-random buffers to the same
 * entry point, so the harness can be stress-run under plain ASan/UBSan
 * on toolchains lacking the libFuzzer runtime (e.g. Apple clang). */

#ifndef CSVNC_FUZZ_MAIN_H
#define CSVNC_FUZZ_MAIN_H

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#ifndef CSVNC_LIBFUZZER
#include <stdio.h>
#include <stdlib.h>

static uint64_t fz_rng = 0x9e3779b97f4a7c15ull;

static uint8_t fz_raw(void)
{
    fz_rng ^= fz_rng << 13;
    fz_rng ^= fz_rng >> 7;
    fz_rng ^= fz_rng << 17;
    return (uint8_t)(fz_rng >> 24);
}

/* Half the bytes come from the values that steer the RFB parser
 * (message types, small counts, the version string, sign bits) so the
 * blind driver still reaches the deeper states. */
static uint8_t fz_byte(void)
{
    static const uint8_t interesting[] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 16, 32, 0x7f, 0x80, 0xfe, 0xff,
        'R', 'F', 'B', ' ', '0', '3', '7', '8', '.', '\n', 0x20, 0x18,
    };
    uint8_t r = fz_raw();

    if (r & 1)
        return interesting[fz_raw() % sizeof(interesting)];
    return fz_raw();
}

int main(int argc, char **argv)
{
    uint8_t buf[512];
    long runs = (argc > 1) ? atol(argv[1]) : 2000000;
    long i;

    for (i = 0; i < runs; i++) {
        size_t n = fz_byte() % (sizeof(buf) + 1), j;

        for (j = 0; j < n; j++)
            buf[j] = fz_byte();
        LLVMFuzzerTestOneInput(buf, n);
    }
    printf("standalone fuzz: %ld runs ok\n", runs);
    return 0;
}
#endif /* !CSVNC_LIBFUZZER */

#endif /* CSVNC_FUZZ_MAIN_H */
