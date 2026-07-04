/* Shared libFuzzer entry declaration plus a standalone driver.
 *
 * Built with -DCSMB_LIBFUZZER (clang -fsanitize=fuzzer), libFuzzer
 * provides main() and calls LLVMFuzzerTestOneInput.  Without that define
 * a small standalone main() feeds pseudo-random buffers to the same
 * entry point, so the harness can be stress-run under plain ASan/UBSan
 * on toolchains lacking the libFuzzer runtime (e.g. Apple clang). */

#ifndef CSMB_FUZZ_MAIN_H
#define CSMB_FUZZ_MAIN_H

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#ifndef CSMB_LIBFUZZER
#include <stdio.h>
#include <stdlib.h>

static uint64_t fz_rng = 0x9e3779b97f4a7c15ull;

static uint8_t fz_byte(void)
{
    fz_rng ^= fz_rng << 13;
    fz_rng ^= fz_rng >> 7;
    fz_rng ^= fz_rng << 17;
    return (uint8_t)(fz_rng >> 24);
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
#endif /* !CSMB_LIBFUZZER */

#endif /* CSMB_FUZZ_MAIN_H */
