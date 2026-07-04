/* Minimal self-test harness for the csmb C unit tests, modeled on the
 * lws api-test style: each test_*() returns a failure count; main sums
 * them and exits non-zero on any failure. */

#ifndef CSMB_TEST_UTIL_H_INCLUDED
#define CSMB_TEST_UTIL_H_INCLUDED

#include <stdio.h>
#include <string.h>

static int csmb_test_failures;

#define TFAIL(...) do {                                         \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);    \
        fprintf(stderr, __VA_ARGS__);                           \
        fprintf(stderr, "\n");                                  \
        csmb_test_failures++;                                   \
    } while (0)

#define TCHECK(expr) do {                       \
        if (!(expr))                            \
            TFAIL("%s", #expr);                 \
    } while (0)

#define TCHECK_EQ(a, b) do {                                            \
        long long va_ = (long long)(a), vb_ = (long long)(b);           \
        if (va_ != vb_)                                                 \
            TFAIL("%s == %s: %lld != %lld", #a, #b, va_, vb_);          \
    } while (0)

#define TEST_MAIN(...) \
    int main(void)                                                      \
    {                                                                   \
        void (*tests[])(void) = { __VA_ARGS__ };                        \
        size_t i;                                                       \
        for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)          \
            tests[i]();                                                 \
        if (csmb_test_failures) {                                       \
            fprintf(stderr, "%d FAILURE(S)\n", csmb_test_failures);     \
            return 1;                                                   \
        }                                                               \
        printf("OK %s\n", __FILE__);                                    \
        return 0;                                                       \
    }

#endif
