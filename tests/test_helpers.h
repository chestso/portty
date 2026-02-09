#ifndef TEST_HELPERS_H
#define TEST_HELPERS_H

#include <stdio.h>

static int test_pass_count = 0;
static int test_fail_count = 0;

#define ASSERT_TRUE(expr)                                                       \
    do {                                                                        \
        if (!(expr)) {                                                          \
            fprintf(stderr, "  FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            test_fail_count++;                                                  \
            return;                                                             \
        }                                                                       \
    } while (0)

#define ASSERT_EQ(a, b)                                                             \
    do {                                                                            \
        if ((a) != (b)) {                                                           \
            fprintf(stderr, "  FAIL: %s:%d: %s == %s (%ld != %ld)\n", __FILE__,    \
                    __LINE__, #a, #b, (long)(a), (long)(b));                        \
            test_fail_count++;                                                      \
            return;                                                                 \
        }                                                                           \
    } while (0)

#define RUN_TEST(fn)                                                \
    do {                                                            \
        int _before = test_fail_count;                              \
        fn();                                                       \
        if (test_fail_count == _before) {                           \
            printf("  PASS: %s\n", #fn);                            \
            test_pass_count++;                                      \
        } else {                                                    \
            printf("  FAIL: %s\n", #fn);                            \
        }                                                           \
    } while (0)

#define TEST_SUMMARY()                                              \
    do {                                                            \
        printf("\n%d passed, %d failed\n",                          \
               test_pass_count, test_fail_count);                   \
        return test_fail_count > 0 ? 1 : 0;                        \
    } while (0)

#endif // TEST_HELPERS_H
