#include "bitmap.h"

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

typedef struct { Bitmap *bitmap; uint64_t fresh; } Producer;
static void *mark_all(void *opaque)
{
    Producer *p = opaque;
    for (unsigned repeat = 0; repeat < 20; ++repeat)
        for (uint64_t i = 0; i < p->bitmap->bit_count; ++i)
            p->fresh += bitmap_set_new_atomic(p->bitmap, i);
    return NULL;
}

#define CHECK(condition)                                                   \
    do {                                                                   \
        if (!(condition)) {                                                \
            fprintf(stderr, "bitmap test failed at line %d: %s\n",       \
                    __LINE__, #condition);                                 \
            bitmap_destroy(&bitmap);                                       \
            return EXIT_FAILURE;                                           \
        }                                                                  \
    } while (0)

int main(void)
{
    static const uint64_t expected[] = {0, 63, 64, 127, 255, 256, 299};
    Bitmap bitmap = {0};
    uint64_t found;
    uint64_t first = 0;
    size_t i;
    CHECK(bitmap_create(&bitmap, 300));
    for (i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i)
        bitmap_set(&bitmap, expected[i]);
    for (i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        CHECK(bitmap_find_next(&bitmap, first, &found));
        CHECK(found == expected[i]);
        CHECK(bitmap_test(&bitmap, found));
        first = found + 1;
    }
    CHECK(!bitmap_find_next(&bitmap, first, &found));
    for (uint64_t from = 0; from <= 300; ++from)
        for (uint64_t end = from; end <= 300; ++end) {
            uint64_t expected_index = from;
            while (expected_index < end && !bitmap_test(&bitmap, expected_index))
                ++expected_index;
            bool have = bitmap_find_next_range(&bitmap, from, end, &found);
            CHECK(have == (expected_index < end));
            CHECK(!have || found == expected_index);
        }

    bitmap_clear_range(&bitmap, 64, 256);
    CHECK(bitmap_test(&bitmap, 0));
    CHECK(bitmap_test(&bitmap, 63));
    CHECK(!bitmap_test(&bitmap, 64));
    CHECK(!bitmap_test(&bitmap, 127));
    CHECK(!bitmap_test(&bitmap, 255));
    CHECK(bitmap_test(&bitmap, 256));
    CHECK(bitmap_test(&bitmap, 299));
    bitmap_set_atomic(&bitmap, 42);
    CHECK(bitmap_test(&bitmap, 42));
    bitmap_unset(&bitmap, 42);
    CHECK(!bitmap_test(&bitmap, 42));
    bitmap_set(&bitmap, 42);
    bitmap_clear_range(&bitmap, 63, 257);
    CHECK(bitmap_test(&bitmap, 0));
    CHECK(!bitmap_test(&bitmap, 63));
    CHECK(!bitmap_test(&bitmap, 256));
    CHECK(bitmap_test(&bitmap, 42));
    CHECK(bitmap_test(&bitmap, 299));
    bitmap_clear(&bitmap);
    CHECK(!bitmap_find_next(&bitmap, 0, &found));
    CHECK(!bitmap_find_next_range(&bitmap, 0, 64, &found));
    bitmap_set(&bitmap, 299);
    CHECK(!bitmap_find_next_range(&bitmap, 0, 299, &found));
    bitmap_clear(&bitmap);
    Producer producers[8] = {0};
    pthread_t threads[8];
    for (unsigned t = 0; t < 8; ++t) {
        producers[t].bitmap = &bitmap;
        CHECK(pthread_create(&threads[t], NULL, mark_all, &producers[t]) == 0);
    }
    uint64_t fresh = 0;
    for (unsigned t = 0; t < 8; ++t) {
        CHECK(pthread_join(threads[t], NULL) == 0);
        fresh += producers[t].fresh;
    }
    CHECK(fresh == 300);
    bitmap_destroy(&bitmap);
    puts("bitmap tests passed");
    return 0;
}
