#ifndef TEST_COMMON_H
#define TEST_COMMON_H

#include <stdio.h>
#include <stdlib.h>

#define CHECK(expr)                                                               \
    do {                                                                          \
        if (!(expr)) {                                                            \
            fprintf(stderr, "CHECK FAILED (%s:%d): %s\n", __FILE__, __LINE__, #expr); \
            exit(1);                                                              \
        }                                                                         \
    } while (0)

#endif /* TEST_COMMON_H */
