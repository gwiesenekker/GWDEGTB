#include "bitmap.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    static const uint64_t expected[] = {0, 63, 64, 127, 255, 256, 299};
    Bitmap bitmap = {0};
    uint64_t found;
    uint64_t first = 0;
    size_t i;
    assert(bitmap_create(&bitmap, 300));
    for (i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i)
        bitmap_set(&bitmap, expected[i]);
    for (i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        assert(bitmap_find_next(&bitmap, first, &found));
        assert(found == expected[i]);
        assert(bitmap_test(&bitmap, found));
        first = found + 1;
    }
    assert(!bitmap_find_next(&bitmap, first, &found));

    bitmap_clear_range(&bitmap, 64, 256);
    assert(bitmap_test(&bitmap, 0));
    assert(bitmap_test(&bitmap, 63));
    assert(!bitmap_test(&bitmap, 64));
    assert(!bitmap_test(&bitmap, 127));
    assert(!bitmap_test(&bitmap, 255));
    assert(bitmap_test(&bitmap, 256));
    assert(bitmap_test(&bitmap, 299));
    bitmap_clear_range(&bitmap, 63, 257);
    assert(bitmap_test(&bitmap, 0));
    assert(!bitmap_test(&bitmap, 63));
    assert(!bitmap_test(&bitmap, 256));
    assert(bitmap_test(&bitmap, 299));
    bitmap_clear(&bitmap);
    assert(!bitmap_find_next(&bitmap, 0, &found));
    bitmap_destroy(&bitmap);
    puts("bitmap tests passed");
    return 0;
}
