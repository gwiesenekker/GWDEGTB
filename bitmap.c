#include "bitmap.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

bool bitmap_create(Bitmap *bitmap, uint64_t bit_count)
{
    uint64_t word_count;
    if (bitmap == NULL || bit_count == 0)
        return false;
    word_count = (bit_count + 63u) / 64u;
    if (word_count > SIZE_MAX / sizeof(*bitmap->words))
        return false;
    bitmap->words = calloc((size_t)word_count, sizeof(*bitmap->words));
    if (bitmap->words == NULL)
        return false;
    bitmap->bit_count = bit_count;
    bitmap->word_count = (size_t)word_count;
    return true;
}

void bitmap_destroy(Bitmap *bitmap)
{
    if (bitmap == NULL)
        return;
    free(bitmap->words);
    memset(bitmap, 0, sizeof(*bitmap));
}

void bitmap_clear(Bitmap *bitmap)
{
    assert(bitmap != NULL && bitmap->words != NULL);
    memset(bitmap->words, 0, bitmap->word_count * sizeof(*bitmap->words));
}

void bitmap_clear_range(Bitmap *bitmap, uint64_t first, uint64_t end)
{
    size_t first_word;
    size_t last_word;
    unsigned first_bit;
    unsigned end_bit;
    assert(bitmap != NULL && bitmap->words != NULL);
    assert(first <= end && end <= bitmap->bit_count);
    if (first == end)
        return;
    first_word = (size_t)(first >> 6);
    last_word = (size_t)((end - 1) >> 6);
    first_bit = (unsigned)(first & 63u);
    end_bit = (unsigned)(end & 63u);
    if (first_word == last_word) {
        uint64_t low = UINT64_MAX << first_bit;
        uint64_t high = end_bit == 0 ? UINT64_MAX :
                                              (UINT64_C(1) << end_bit) - 1;
        bitmap->words[first_word] &= ~(low & high);
        return;
    }
    bitmap->words[first_word] &= ~(UINT64_MAX << first_bit);
    if (last_word > first_word + 1)
        memset(bitmap->words + first_word + 1, 0,
               (last_word - first_word - 1) * sizeof(*bitmap->words));
    if (end_bit == 0)
        bitmap->words[last_word] = 0;
    else
        bitmap->words[last_word] &= ~((UINT64_C(1) << end_bit) - 1);
}

void bitmap_set(Bitmap *bitmap, uint64_t index)
{
    assert(bitmap != NULL && bitmap->words != NULL);
    assert(index < bitmap->bit_count);
    bitmap->words[index >> 6] |= UINT64_C(1) << (index & 63u);
}

void bitmap_unset(Bitmap *bitmap, uint64_t index)
{
    assert(bitmap != NULL && bitmap->words != NULL);
    assert(index < bitmap->bit_count);
    bitmap->words[index >> 6] &= ~(UINT64_C(1) << (index & 63u));
}

void bitmap_set_atomic(Bitmap *bitmap, uint64_t index)
{
    uint64_t mask;
    assert(bitmap != NULL && bitmap->words != NULL);
    assert(index < bitmap->bit_count);
    mask = UINT64_C(1) << (index & 63u);
#if defined(__GNUC__) || defined(__clang__)
    __atomic_fetch_or(&bitmap->words[index >> 6], mask, __ATOMIC_RELAXED);
#else
    /* The threaded generator requires a compiler with atomic intrinsics. */
    bitmap->words[index >> 6] |= mask;
#endif
}

bool bitmap_test(const Bitmap *bitmap, uint64_t index)
{
    assert(bitmap != NULL && bitmap->words != NULL);
    assert(index < bitmap->bit_count);
    return (bitmap->words[index >> 6] &
            (UINT64_C(1) << (index & 63u))) != 0;
}

bool bitmap_find_next(const Bitmap *bitmap, uint64_t first, uint64_t *found)
{
    size_t word_index;
    uint64_t word;
    assert(bitmap != NULL && bitmap->words != NULL && found != NULL);
    if (first >= bitmap->bit_count)
        return false;
    word_index = (size_t)(first >> 6);
    word = bitmap->words[word_index] & (UINT64_MAX << (first & 63u));
    while (word == 0) {
        if (++word_index == bitmap->word_count)
            return false;
        word = bitmap->words[word_index];
    }
#if defined(__GNUC__) || defined(__clang__)
    *found = (uint64_t)word_index * 64u +
             (uint64_t)__builtin_ctzll(word);
#else
    {
        unsigned bit = 0;
        while ((word & UINT64_C(1)) == 0) {
            word >>= 1;
            ++bit;
        }
        *found = (uint64_t)word_index * 64u + bit;
    }
#endif
    return *found < bitmap->bit_count;
}
