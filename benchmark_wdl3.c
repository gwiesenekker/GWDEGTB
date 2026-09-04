#define _POSIX_C_SOURCE 200809L

#include "gwdegtb.h"
#include "wdl.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <zstd.h>

#define WDL3_PACKED_BYTES ((WDL_PAGE_SIZE * 2 * 3) / 8)
#define WDL3_MAX_TRANSFORMED (WDL3_PACKED_BYTES + 2 + WDL_PAGE_SIZE * 4)
#define SAMPLE_LIMIT 8192
#define DECODE_BENCHMARK_BYTES (UINT64_C(512) << 20)

typedef enum {
    CODEC_IMPLICIT,
    CODEC_WDL4,
    CODEC_WDL3_FIXED,
    CODEC_WDL3_DELTA
} PageCodec;

typedef struct {
    uint64_t page;
    PageCodec codec;
    unsigned char *current;
    size_t current_size;
    unsigned char *hybrid;
    size_t hybrid_size;
    unsigned char raw[WDL_PAGE_SIZE];
} PageSample;

static const uint8_t nibble_to_code[16] = {
    0, 1, 2, 0, 3, 4, 5, 0, 6, 7, 0, 0, 0, 0, 0, 0
};

static const uint8_t code_to_nibble[8] = {0, 1, 2, 4, 5, 6, 8, 9};

static double now_seconds(void)
{
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return value.tv_sec + value.tv_nsec * 1e-9;
}

static void load_page(const unsigned char *bitmap, size_t bitmap_bytes,
                      uint64_t page_number,
                      unsigned char page[WDL_PAGE_SIZE])
{
    uint64_t offset = page_number * WDL_PAGE_SIZE;
    size_t available = offset < bitmap_bytes ? bitmap_bytes - (size_t)offset : 0;
    if (available > WDL_PAGE_SIZE)
        available = WDL_PAGE_SIZE;
    memset(page, 0, WDL_PAGE_SIZE);
    if (available != 0)
        memcpy(page, bitmap + offset, available);
}

static bool page_is_zero(const unsigned char page[WDL_PAGE_SIZE])
{
    for (size_t byte = 0; byte < WDL_PAGE_SIZE; ++byte)
        if (page[byte] != 0)
            return false;
    return true;
}

static void pack_code(unsigned char packed[WDL3_PACKED_BYTES],
                      size_t position, uint8_t code)
{
    size_t bit = position * 3;
    unsigned shift = bit & 7;
    packed[bit >> 3] |= code << shift;
    if (shift > 5)
        packed[(bit >> 3) + 1] |= code >> (8 - shift);
}

static uint8_t unpack_code(const unsigned char packed[WDL3_PACKED_BYTES],
                           size_t position)
{
    size_t bit = position * 3;
    unsigned shift = bit & 7;
    uint16_t value = packed[bit >> 3];
    if (shift > 5)
        value |= (uint16_t)packed[(bit >> 3) + 1] << 8;
    return (value >> shift) & 7;
}

static size_t put_varint(unsigned char *destination, uint16_t value)
{
    size_t bytes = 0;
    do {
        unsigned char byte = value & 0x7f;
        value >>= 7;
        if (value != 0)
            byte |= 0x80;
        destination[bytes++] = byte;
    } while (value != 0);
    return bytes;
}

static bool get_varint(const unsigned char *source, size_t size,
                       size_t *offset, uint16_t *value)
{
    uint16_t result = 0;
    unsigned shift = 0;
    while (*offset < size && shift <= 14) {
        unsigned char byte = source[(*offset)++];
        result |= (uint16_t)(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) {
            *value = result;
            return true;
        }
        shift += 7;
    }
    return false;
}

static size_t transform_page(const unsigned char page[WDL_PAGE_SIZE],
                             bool delta,
                             unsigned char transformed[WDL3_MAX_TRANSFORMED],
                             uint16_t *exception_count)
{
    uint16_t exceptions[WDL_PAGE_SIZE * 2];
    uint16_t count = 0;
    memset(transformed, 0, WDL3_PACKED_BYTES);
    for (size_t position = 0; position < WDL_PAGE_SIZE * 2; ++position) {
        uint8_t nibble =
            (page[position / 2] >> (4 * (position & 1))) & 15;
        if (nibble == 10)
            exceptions[count++] = (uint16_t)position;
        pack_code(transformed, position, nibble_to_code[nibble]);
    }
    size_t output = WDL3_PACKED_BYTES;
    transformed[output++] = count & 0xff;
    transformed[output++] = count >> 8;
    if (!delta) {
        for (uint16_t entry = 0; entry < count; ++entry) {
            transformed[output++] = exceptions[entry] & 0xff;
            transformed[output++] = exceptions[entry] >> 8;
        }
    } else {
        uint16_t previous = 0;
        for (uint16_t entry = 0; entry < count; ++entry) {
            uint16_t difference = exceptions[entry] - previous;
            output += put_varint(transformed + output, difference);
            previous = exceptions[entry];
        }
    }
    *exception_count = count;
    return output;
}

static bool restore_page(const unsigned char *transformed, size_t size,
                         bool delta,
                         unsigned char page[WDL_PAGE_SIZE])
{
    if (size < WDL3_PACKED_BYTES + 2)
        return false;
    memset(page, 0, WDL_PAGE_SIZE);
    for (size_t position = 0; position < WDL_PAGE_SIZE * 2; ++position) {
        uint8_t nibble = code_to_nibble[unpack_code(transformed, position)];
        page[position / 2] |= nibble << (4 * (position & 1));
    }
    size_t input = WDL3_PACKED_BYTES;
    uint16_t count = transformed[input] |
                     (uint16_t)transformed[input + 1] << 8;
    input += 2;
    uint16_t previous = 0;
    for (uint16_t entry = 0; entry < count; ++entry) {
        uint16_t position;
        if (!delta) {
            if (input + 2 > size)
                return false;
            position = transformed[input] |
                       (uint16_t)transformed[input + 1] << 8;
            input += 2;
        } else {
            uint16_t difference;
            if (!get_varint(transformed, size, &input, &difference) ||
                difference > UINT16_MAX - previous)
                return false;
            position = previous + difference;
            previous = position;
        }
        if (position >= WDL_PAGE_SIZE * 2)
            return false;
        size_t byte = position / 2;
        unsigned shift = 4 * (position & 1);
        page[byte] = (page[byte] & ~(15u << shift)) | (10u << shift);
    }
    return input == size;
}

static size_t compress_buffer(ZSTD_CCtx *compressor,
                              const void *source, size_t source_size,
                              unsigned char *destination,
                              size_t destination_capacity)
{
    return ZSTD_compressCCtx(compressor, destination, destination_capacity,
                             source, source_size, 3);
}

static void free_samples(PageSample *samples, size_t count)
{
    if (samples == NULL)
        return;
    for (size_t sample = 0; sample < count; ++sample) {
        free(samples[sample].current);
        free(samples[sample].hybrid);
    }
    free(samples);
}

static bool decode_hybrid(ZSTD_DCtx *decompressor, const PageSample *sample,
                          unsigned char transformed[WDL3_MAX_TRANSFORMED],
                          unsigned char page[WDL_PAGE_SIZE])
{
    if (sample->codec == CODEC_IMPLICIT) {
        memset(page, 0, WDL_PAGE_SIZE);
        return true;
    }
    if (sample->codec == CODEC_WDL4) {
        size_t expanded = ZSTD_decompressDCtx(
            decompressor, page, WDL_PAGE_SIZE,
            sample->hybrid, sample->hybrid_size);
        return !ZSTD_isError(expanded) && expanded == WDL_PAGE_SIZE;
    }
    size_t expanded = ZSTD_decompressDCtx(
        decompressor, transformed, WDL3_MAX_TRANSFORMED,
        sample->hybrid, sample->hybrid_size);
    return !ZSTD_isError(expanded) &&
           restore_page(transformed, expanded,
                        sample->codec == CODEC_WDL3_DELTA, page);
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s DIRECTORY DATABASE\n", argv[0]);
        return EXIT_FAILURE;
    }
    const char *directory = argv[1], *name = argv[2];
    uint64_t maximum_index = 0;
    size_t bitmap_bytes = 0, existing_file_bytes = 0, sample_count = 0;
    unsigned char *bitmap = NULL, *compressed4 = NULL,
                  *compressed_fixed = NULL, *compressed_delta = NULL;
    PageSample *samples = NULL;
    ZSTD_CCtx *compressor = NULL;
    ZSTD_DCtx *decompressor = NULL;
    int result = EXIT_FAILURE;

    if (!gwdegtb_wdl_info(name, &maximum_index, &bitmap_bytes) ||
        !gwdegtb_wdl_compressed_info(directory, name, &existing_file_bytes)) {
        fprintf(stderr, "cannot inspect WDL: %s\n", gwdegtb_last_error());
        goto done;
    }
    bitmap = malloc(bitmap_bytes);
    compressor = ZSTD_createCCtx();
    decompressor = ZSTD_createDCtx();
    size_t compressed_capacity = ZSTD_compressBound(WDL3_MAX_TRANSFORMED);
    compressed4 = malloc(compressed_capacity);
    compressed_fixed = malloc(compressed_capacity);
    compressed_delta = malloc(compressed_capacity);
    if (bitmap == NULL || compressor == NULL || decompressor == NULL ||
        compressed4 == NULL || compressed_fixed == NULL ||
        compressed_delta == NULL ||
        !gwdegtb_wdl_decompress_threads(directory, name, bitmap,
                                        bitmap_bytes, 4)) {
        fprintf(stderr, "cannot prepare WDL: %s\n", gwdegtb_last_error());
        goto done;
    }

    uint64_t page_count = bitmap_bytes / WDL_PAGE_SIZE +
                          (bitmap_bytes % WDL_PAGE_SIZE != 0);
    uint64_t implicit_pages = 0, exception_total = 0,
             chosen4 = 0, chosen_fixed = 0, chosen_delta = 0;
    uint64_t payload4 = 0, payload_fixed = 0, payload_delta = 0,
             payload_hybrid = 0;
    unsigned char page[WDL_PAGE_SIZE];
    unsigned char fixed[WDL3_MAX_TRANSFORMED];
    unsigned char delta[WDL3_MAX_TRANSFORMED];
    double start = now_seconds();
    for (uint64_t page_number = 0; page_number < page_count; ++page_number) {
        load_page(bitmap, bitmap_bytes, page_number, page);
        if (page_is_zero(page)) {
            ++implicit_pages;
            continue;
        }
        uint16_t fixed_count, delta_count;
        size_t fixed_size = transform_page(page, false, fixed, &fixed_count);
        size_t delta_size = transform_page(page, true, delta, &delta_count);
        if (fixed_count != delta_count)
            goto done;
        exception_total += fixed_count;
        size_t size4 = compress_buffer(compressor, page, sizeof(page),
                                       compressed4, compressed_capacity);
        size_t size_fixed = compress_buffer(compressor, fixed, fixed_size,
                                            compressed_fixed,
                                            compressed_capacity);
        size_t size_delta = compress_buffer(compressor, delta, delta_size,
                                            compressed_delta,
                                            compressed_capacity);
        if (ZSTD_isError(size4) || ZSTD_isError(size_fixed) ||
            ZSTD_isError(size_delta))
            goto done;
        payload4 += size4;
        payload_fixed += size_fixed;
        payload_delta += size_delta;
        if (size4 <= size_fixed && size4 <= size_delta) {
            payload_hybrid += size4;
            ++chosen4;
        } else if (size_fixed <= size_delta) {
            payload_hybrid += size_fixed;
            ++chosen_fixed;
        } else {
            payload_hybrid += size_delta;
            ++chosen_delta;
        }
    }
    double compression_time = now_seconds() - start;

    sample_count = page_count < SAMPLE_LIMIT
                       ? (size_t)page_count : SAMPLE_LIMIT;
    samples = calloc(sample_count, sizeof(*samples));
    if (samples == NULL)
        goto done;
    for (size_t sample = 0; sample < sample_count; ++sample) {
        PageSample *entry = &samples[sample];
        entry->page = (uint64_t)sample * page_count / sample_count;
        load_page(bitmap, bitmap_bytes, entry->page, entry->raw);
        if (page_is_zero(entry->raw)) {
            entry->codec = CODEC_IMPLICIT;
            continue;
        }
        uint16_t ignored;
        size_t fixed_size = transform_page(entry->raw, false, fixed, &ignored);
        size_t delta_size = transform_page(entry->raw, true, delta, &ignored);
        size_t size4 = compress_buffer(compressor, entry->raw,
                                       sizeof(entry->raw), compressed4,
                                       compressed_capacity);
        size_t size_fixed = compress_buffer(compressor, fixed, fixed_size,
                                            compressed_fixed,
                                            compressed_capacity);
        size_t size_delta = compress_buffer(compressor, delta, delta_size,
                                            compressed_delta,
                                            compressed_capacity);
        if (ZSTD_isError(size4) || ZSTD_isError(size_fixed) ||
            ZSTD_isError(size_delta))
            goto done;
        entry->current = malloc(size4);
        if (entry->current == NULL)
            goto done;
        memcpy(entry->current, compressed4, size4);
        entry->current_size = size4;
        const unsigned char *selected;
        if (size4 <= size_fixed && size4 <= size_delta) {
            entry->codec = CODEC_WDL4;
            entry->hybrid_size = size4;
            selected = compressed4;
        } else if (size_fixed <= size_delta) {
            entry->codec = CODEC_WDL3_FIXED;
            entry->hybrid_size = size_fixed;
            selected = compressed_fixed;
        } else {
            entry->codec = CODEC_WDL3_DELTA;
            entry->hybrid_size = size_delta;
            selected = compressed_delta;
        }
        entry->hybrid = malloc(entry->hybrid_size);
        if (entry->hybrid == NULL)
            goto done;
        memcpy(entry->hybrid, selected, entry->hybrid_size);
    }

    unsigned char decoded[WDL_PAGE_SIZE];
    unsigned char transformed[WDL3_MAX_TRANSFORMED];
    for (size_t sample = 0; sample < sample_count; ++sample) {
        PageSample *entry = &samples[sample];
        if (!decode_hybrid(decompressor, entry, transformed, decoded) ||
            memcmp(decoded, entry->raw, WDL_PAGE_SIZE) != 0)
            goto done;
    }
    uint64_t rounds = DECODE_BENCHMARK_BYTES /
                      (sample_count * (uint64_t)WDL_PAGE_SIZE);
    if (rounds == 0)
        rounds = 1;
    volatile uint64_t checksum = 0;
    start = now_seconds();
    for (uint64_t round = 0; round < rounds; ++round)
        for (size_t sample = 0; sample < sample_count; ++sample) {
            PageSample *entry = &samples[sample];
            if (entry->codec == CODEC_IMPLICIT)
                memset(decoded, 0, sizeof(decoded));
            else {
                size_t expanded = ZSTD_decompressDCtx(
                    decompressor, decoded, sizeof(decoded),
                    entry->current, entry->current_size);
                if (ZSTD_isError(expanded) || expanded != sizeof(decoded))
                    goto done;
            }
            checksum += decoded[(sample + round) & (WDL_PAGE_SIZE - 1)];
        }
    double current_decode_time = now_seconds() - start;
    start = now_seconds();
    for (uint64_t round = 0; round < rounds; ++round)
        for (size_t sample = 0; sample < sample_count; ++sample) {
            PageSample *entry = &samples[sample];
            if (!decode_hybrid(decompressor, entry, transformed, decoded))
                goto done;
            checksum += decoded[(sample + round) & (WDL_PAGE_SIZE - 1)];
        }
    double hybrid_decode_time = now_seconds() - start;

    uint64_t directory_bytes = page_count * 14 + 64;
    uint64_t file4 = payload4 + directory_bytes;
    uint64_t file_fixed = payload_fixed + directory_bytes;
    uint64_t file_delta = payload_delta + directory_bytes;
    uint64_t file_hybrid = payload_hybrid + directory_bytes;
    printf("database: %s\n", name);
    printf("positions: %" PRIu64 " raw-packed: %.2f MiB pages: %" PRIu64
           " implicit-draw: %" PRIu64 "\n",
           maximum_index + 1, bitmap_bytes / 1048576.0,
           page_count, implicit_pages);
    printf("loss/loss exceptions: %" PRIu64 " (%.5f%%, %.3f/page)\n",
           exception_total,
           100.0 * exception_total / (maximum_index + 1),
           (double)exception_total / page_count);
    printf("current 4-bit + Zstd:       %.2f MiB, %.2f%% raw\n",
           file4 / 1048576.0, 100.0 * file4 / bitmap_bytes);
    printf("3-bit + fixed exceptions:  %.2f MiB, %.2f%% raw\n",
           file_fixed / 1048576.0,
           100.0 * file_fixed / bitmap_bytes);
    printf("3-bit + delta exceptions:  %.2f MiB, %.2f%% raw\n",
           file_delta / 1048576.0,
           100.0 * file_delta / bitmap_bytes);
    printf("per-page hybrid:            %.2f MiB, %.2f%% raw, "
           "saved %.2f%%\n",
           file_hybrid / 1048576.0,
           100.0 * file_hybrid / bitmap_bytes,
           100.0 * (file4 - file_hybrid) / file4);
    printf("hybrid pages: 4-bit=%" PRIu64 " fixed=%" PRIu64
           " delta=%" PRIu64 "; three encodings compressed in %.3f s\n",
           chosen4, chosen_fixed, chosen_delta, compression_time);
    printf("existing WDL file:          %.2f MiB, %.2f%% raw\n",
           existing_file_bytes / 1048576.0,
           100.0 * existing_file_bytes / bitmap_bytes);
    uint64_t decoded_bytes = rounds * sample_count * WDL_PAGE_SIZE;
    printf("decode benchmark: %.2f MiB, %zu sampled pages, checksum=%" PRIu64
           "\n", decoded_bytes / 1048576.0, sample_count, checksum);
    printf("current decode: %.3f s, %.1f MiB/s\n", current_decode_time,
           decoded_bytes / 1048576.0 / current_decode_time);
    printf("hybrid decode:  %.3f s, %.1f MiB/s, %.2fx current time\n",
           hybrid_decode_time,
           decoded_bytes / 1048576.0 / hybrid_decode_time,
           hybrid_decode_time / current_decode_time);
    result = EXIT_SUCCESS;
done:
    free_samples(samples, sample_count);
    ZSTD_freeCCtx(compressor);
    ZSTD_freeDCtx(decompressor);
    free(compressed4);
    free(compressed_fixed);
    free(compressed_delta);
    free(bitmap);
    return result;
}
