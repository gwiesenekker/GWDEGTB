#ifndef BITMAP_H
#define BITMAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t *words;
    uint64_t bit_count;
    size_t word_count;
} Bitmap;

bool bitmap_create(Bitmap *bitmap, uint64_t bit_count);
void bitmap_destroy(Bitmap *bitmap);
void bitmap_clear(Bitmap *bitmap);
void bitmap_clear_range(Bitmap *bitmap, uint64_t first, uint64_t end);
void bitmap_set(Bitmap *bitmap, uint64_t index);
bool bitmap_test(const Bitmap *bitmap, uint64_t index);
bool bitmap_find_next(const Bitmap *bitmap, uint64_t first, uint64_t *found);

#endif
