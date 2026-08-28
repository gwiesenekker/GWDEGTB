#include "bitmap.h"

#include <stdio.h>
#include <stdlib.h>

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
    bitmap_destroy(&bitmap);
    puts("bitmap tests passed");
    return 0;
}
