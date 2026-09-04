#define _POSIX_C_SOURCE 200809L

#include "gwdegtb.h"
#include "wdl.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <zstd.h>

#define TUNSTALL_SYMBOLS 16
#define TUNSTALL_MAX_CODES 65536
#define INVALID_NODE UINT32_MAX
#define SAMPLE_LIMIT 8192
#define DECODE_BENCHMARK_BYTES (UINT64_C(512) << 20)

typedef struct {
    uint32_t child[TUNSTALL_SYMBOLS];
    uint32_t parent;
    double log_probability;
    uint16_t code;
    uint8_t symbol;
    bool leaf;
} TunstallNode;

typedef struct {
    TunstallNode *nodes;
    uint32_t node_count;
    uint32_t node_capacity;
    uint32_t *heap;
    uint32_t heap_size;
    uint32_t leaf_count;
    uint32_t maximum_codes;
    unsigned code_bytes;
    uint8_t symbols[TUNSTALL_SYMBOLS];
    unsigned symbol_count;
    uint8_t padding_symbol;
    uint32_t *phrase_offsets;
    uint16_t *phrase_lengths;
    uint8_t *phrases;
    size_t phrase_bytes;
} TunstallDictionary;

typedef enum {
    SAMPLE_IMPLICIT,
    SAMPLE_TUNSTALL,
    SAMPLE_RAW
} SampleMode;

typedef struct {
    uint64_t page;
    SampleMode mode;
    unsigned char *codes;
    size_t code_count;
    unsigned char *zstd;
    size_t zstd_size;
    unsigned char raw[WDL_PAGE_SIZE];
} PageSample;

static double now_seconds(void)
{
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return value.tv_sec + value.tv_nsec * 1e-9;
}

static bool heap_higher(const TunstallDictionary *dictionary,
                        uint32_t left, uint32_t right)
{
    double a = dictionary->nodes[left].log_probability;
    double b = dictionary->nodes[right].log_probability;
    return a > b || (a == b && left < right);
}

static void heap_push(TunstallDictionary *dictionary, uint32_t node)
{
    uint32_t index = dictionary->heap_size++;
    while (index != 0) {
        uint32_t parent = (index - 1) / 2;
        if (heap_higher(dictionary, dictionary->heap[parent], node))
            break;
        dictionary->heap[index] = dictionary->heap[parent];
        index = parent;
    }
    dictionary->heap[index] = node;
}

static uint32_t heap_pop(TunstallDictionary *dictionary)
{
    uint32_t result = dictionary->heap[0];
    uint32_t replacement = dictionary->heap[--dictionary->heap_size];
    uint32_t index = 0;
    while (index * 2 + 1 < dictionary->heap_size) {
        uint32_t child = index * 2 + 1;
        if (child + 1 < dictionary->heap_size &&
            heap_higher(dictionary, dictionary->heap[child + 1],
                        dictionary->heap[child]))
            ++child;
        if (heap_higher(dictionary, replacement,
                        dictionary->heap[child]))
            break;
        dictionary->heap[index] = dictionary->heap[child];
        index = child;
    }
    if (dictionary->heap_size != 0)
        dictionary->heap[index] = replacement;
    return result;
}

static uint32_t add_node(TunstallDictionary *dictionary, uint32_t parent,
                         uint8_t symbol, double log_probability)
{
    if (dictionary->node_count == dictionary->node_capacity) {
        uint32_t capacity = dictionary->node_capacity == 0
                                ? 1024 : dictionary->node_capacity * 2;
        TunstallNode *nodes = realloc(dictionary->nodes,
                                     capacity * sizeof(*nodes));
        if (nodes == NULL)
            return INVALID_NODE;
        dictionary->nodes = nodes;
        dictionary->node_capacity = capacity;
    }
    uint32_t index = dictionary->node_count++;
    TunstallNode *node = &dictionary->nodes[index];
    for (unsigned child = 0; child < TUNSTALL_SYMBOLS; ++child)
        node->child[child] = INVALID_NODE;
    node->parent = parent;
    node->log_probability = log_probability;
    node->code = 0;
    node->symbol = symbol;
    node->leaf = true;
    return index;
}

static void tunstall_destroy(TunstallDictionary *dictionary)
{
    free(dictionary->nodes);
    free(dictionary->heap);
    free(dictionary->phrase_offsets);
    free(dictionary->phrase_lengths);
    free(dictionary->phrases);
    memset(dictionary, 0, sizeof(*dictionary));
}

static bool tunstall_build(TunstallDictionary *dictionary,
                           const uint64_t frequencies[TUNSTALL_SYMBOLS],
                           unsigned code_bits)
{
    uint64_t total = 0, largest = 0;
    uint32_t root;
    memset(dictionary, 0, sizeof(*dictionary));
    dictionary->maximum_codes = UINT32_C(1) << code_bits;
    dictionary->code_bytes = code_bits / 8;
    for (unsigned symbol = 0; symbol < TUNSTALL_SYMBOLS; ++symbol) {
        total += frequencies[symbol];
        if (frequencies[symbol] != 0)
            dictionary->symbols[dictionary->symbol_count++] = symbol;
        if (frequencies[symbol] > largest) {
            largest = frequencies[symbol];
            dictionary->padding_symbol = symbol;
        }
    }
    if (dictionary->symbol_count < 2 || total == 0)
        return false;
    dictionary->heap = malloc(dictionary->maximum_codes *
                              sizeof(*dictionary->heap));
    if (dictionary->heap == NULL)
        return false;
    root = add_node(dictionary, INVALID_NODE, 0, 0.0);
    if (root == INVALID_NODE)
        return false;
    dictionary->nodes[root].leaf = false;
    for (unsigned i = 0; i < dictionary->symbol_count; ++i) {
        uint8_t symbol = dictionary->symbols[i];
        uint32_t node = add_node(dictionary, root, symbol,
            log((double)frequencies[symbol] / (double)total));
        if (node == INVALID_NODE)
            return false;
        dictionary->nodes[root].child[symbol] = node;
        heap_push(dictionary, node);
    }
    dictionary->leaf_count = dictionary->symbol_count;
    while (dictionary->leaf_count + dictionary->symbol_count - 1 <=
           dictionary->maximum_codes) {
        uint32_t split = heap_pop(dictionary);
        dictionary->nodes[split].leaf = false;
        for (unsigned i = 0; i < dictionary->symbol_count; ++i) {
            uint8_t symbol = dictionary->symbols[i];
            uint32_t child = add_node(
                dictionary, split, symbol,
                dictionary->nodes[split].log_probability +
                log((double)frequencies[symbol] / (double)total));
            if (child == INVALID_NODE)
                return false;
            dictionary->nodes[split].child[symbol] = child;
            heap_push(dictionary, child);
        }
        dictionary->leaf_count += dictionary->symbol_count - 1;
    }

    dictionary->phrase_offsets = calloc(dictionary->leaf_count + 1,
                                         sizeof(*dictionary->phrase_offsets));
    dictionary->phrase_lengths = calloc(dictionary->leaf_count,
                                         sizeof(*dictionary->phrase_lengths));
    if (dictionary->phrase_offsets == NULL ||
        dictionary->phrase_lengths == NULL)
        return false;
    uint32_t code = 0;
    size_t phrase_bytes = 0;
    for (uint32_t node = 1; node < dictionary->node_count; ++node) {
        if (!dictionary->nodes[node].leaf)
            continue;
        uint32_t depth = 0;
        for (uint32_t cursor = node; cursor != root;
             cursor = dictionary->nodes[cursor].parent)
            ++depth;
        if (depth > UINT16_MAX || phrase_bytes > UINT32_MAX - depth)
            return false;
        dictionary->nodes[node].code = (uint16_t)code;
        dictionary->phrase_offsets[code] = (uint32_t)phrase_bytes;
        dictionary->phrase_lengths[code] = (uint16_t)depth;
        phrase_bytes += depth;
        ++code;
    }
    if (code != dictionary->leaf_count)
        return false;
    dictionary->phrase_offsets[code] = (uint32_t)phrase_bytes;
    dictionary->phrases = malloc(phrase_bytes);
    if (dictionary->phrases == NULL)
        return false;
    dictionary->phrase_bytes = phrase_bytes;
    for (uint32_t node = 1; node < dictionary->node_count; ++node) {
        if (!dictionary->nodes[node].leaf)
            continue;
        uint32_t current_code = dictionary->nodes[node].code;
        size_t output = dictionary->phrase_offsets[current_code] +
                        dictionary->phrase_lengths[current_code];
        for (uint32_t cursor = node; cursor != root;
             cursor = dictionary->nodes[cursor].parent)
            dictionary->phrases[--output] = dictionary->nodes[cursor].symbol;
    }
    return true;
}

static size_t tunstall_encode(const TunstallDictionary *dictionary,
                              const unsigned char page[WDL_PAGE_SIZE],
                              uint16_t codes[WDL_PAGE_SIZE * 2])
{
    uint32_t node = 0;
    size_t count = 0;
    for (size_t position = 0; position < WDL_PAGE_SIZE * 2; ++position) {
        uint8_t symbol = (page[position / 2] >> (4 * (position & 1))) & 15;
        node = dictionary->nodes[node].child[symbol];
        if (node == INVALID_NODE)
            return SIZE_MAX;
        if (dictionary->nodes[node].leaf) {
            codes[count++] = dictionary->nodes[node].code;
            node = 0;
        }
    }
    while (node != 0) {
        node = dictionary->nodes[node].child[dictionary->padding_symbol];
        if (node == INVALID_NODE)
            return SIZE_MAX;
        if (dictionary->nodes[node].leaf) {
            codes[count++] = dictionary->nodes[node].code;
            node = 0;
        }
    }
    return count;
}

static bool tunstall_decode(const TunstallDictionary *dictionary,
                            const unsigned char *codes, size_t code_count,
                            unsigned char page[WDL_PAGE_SIZE])
{
    size_t position = 0;
    memset(page, 0, WDL_PAGE_SIZE);
    for (size_t input = 0; input < code_count &&
                           position < WDL_PAGE_SIZE * 2; ++input) {
        uint32_t code = codes[input * dictionary->code_bytes];
        if (dictionary->code_bytes == 2)
            code |= (uint32_t)codes[input * 2 + 1] << 8;
        if (code >= dictionary->leaf_count)
            return false;
        uint32_t offset = dictionary->phrase_offsets[code];
        uint16_t length = dictionary->phrase_lengths[code];
        for (uint16_t phrase = 0; phrase < length &&
                                  position < WDL_PAGE_SIZE * 2; ++phrase) {
            uint8_t symbol = dictionary->phrases[offset + phrase];
            page[position / 2] |= symbol << (4 * (position & 1));
            ++position;
        }
    }
    return position == WDL_PAGE_SIZE * 2;
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

static bool all_zero(const unsigned char page[WDL_PAGE_SIZE])
{
    for (size_t offset = 0; offset < WDL_PAGE_SIZE; ++offset)
        if (page[offset] != 0)
            return false;
    return true;
}

static void free_samples(PageSample *samples, size_t count)
{
    if (samples == NULL)
        return;
    for (size_t sample = 0; sample < count; ++sample) {
        free(samples[sample].codes);
        free(samples[sample].zstd);
    }
    free(samples);
}

int main(int argc, char **argv)
{
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "usage: %s DIRECTORY DATABASE [8|16]\n", argv[0]);
        return EXIT_FAILURE;
    }
    const char *directory = argv[1], *name = argv[2];
    unsigned code_bits = argc == 4 ? (unsigned)strtoul(argv[3], NULL, 10) : 16;
    if (code_bits != 8 && code_bits != 16) {
        fprintf(stderr, "Tunstall code width must be 8 or 16 bits\n");
        return EXIT_FAILURE;
    }
    uint64_t maximum_index = 0;
    size_t bitmap_bytes = 0, existing_file_bytes = 0, sample_count = 0;
    unsigned char *bitmap = NULL;
    TunstallDictionary dictionary;
    uint64_t frequencies[TUNSTALL_SYMBOLS] = {0};
    PageSample *samples = NULL;
    ZSTD_CCtx *compressor = NULL;
    ZSTD_DCtx *decompressor = NULL;
    void *zstd_buffer = NULL;
    int result = EXIT_FAILURE;
    memset(&dictionary, 0, sizeof(dictionary));

    if (!gwdegtb_wdl_info(name, &maximum_index, &bitmap_bytes) ||
        !gwdegtb_wdl_compressed_info(directory, name, &existing_file_bytes)) {
        fprintf(stderr, "cannot inspect WDL: %s\n", gwdegtb_last_error());
        goto done;
    }
    bitmap = malloc(bitmap_bytes);
    if (bitmap == NULL ||
        !gwdegtb_wdl_decompress_threads(directory, name, bitmap,
                                        bitmap_bytes, 4)) {
        fprintf(stderr, "cannot decompress WDL: %s\n",
                gwdegtb_last_error());
        goto done;
    }
    for (size_t byte = 0; byte < bitmap_bytes; ++byte) {
        ++frequencies[bitmap[byte] & 15];
        ++frequencies[bitmap[byte] >> 4];
    }
    double start = now_seconds();
    if (!tunstall_build(&dictionary, frequencies, code_bits)) {
        fprintf(stderr, "cannot construct Tunstall dictionary\n");
        goto done;
    }
    double dictionary_time = now_seconds() - start;
    uint64_t page_count = bitmap_bytes / WDL_PAGE_SIZE +
                          (bitmap_bytes % WDL_PAGE_SIZE != 0);
    sample_count = page_count < SAMPLE_LIMIT
                       ? (size_t)page_count : SAMPLE_LIMIT;
    samples = calloc(sample_count, sizeof(*samples));
    compressor = ZSTD_createCCtx();
    decompressor = ZSTD_createDCtx();
    zstd_buffer = malloc(ZSTD_compressBound(WDL_PAGE_SIZE));
    if (samples == NULL || compressor == NULL || decompressor == NULL ||
        zstd_buffer == NULL)
        goto done;

    uint64_t tunstall_payload = 0, zstd_payload = 0, implicit_pages = 0,
             raw_tunstall_pages = 0;
    uint16_t codes[WDL_PAGE_SIZE * 2];
    unsigned char page[WDL_PAGE_SIZE];
    start = now_seconds();
    for (uint64_t page_number = 0; page_number < page_count; ++page_number) {
        load_page(bitmap, bitmap_bytes, page_number, page);
        if (all_zero(page)) {
            ++implicit_pages;
            continue;
        }
        size_t code_count = tunstall_encode(&dictionary, page, codes);
        if (code_count == SIZE_MAX)
            goto done;
        size_t encoded = code_count * dictionary.code_bytes;
        if (encoded >= WDL_PAGE_SIZE) {
            tunstall_payload += WDL_PAGE_SIZE;
            ++raw_tunstall_pages;
        } else {
            tunstall_payload += encoded;
        }
    }
    double tunstall_compress_time = now_seconds() - start;
    start = now_seconds();
    for (uint64_t page_number = 0; page_number < page_count; ++page_number) {
        load_page(bitmap, bitmap_bytes, page_number, page);
        if (all_zero(page))
            continue;
        size_t compressed = ZSTD_compressCCtx(
            compressor, zstd_buffer, ZSTD_compressBound(WDL_PAGE_SIZE),
            page, WDL_PAGE_SIZE, 3);
        if (ZSTD_isError(compressed))
            goto done;
        zstd_payload += compressed;
    }
    double zstd_compress_time = now_seconds() - start;

    for (size_t sample = 0; sample < sample_count; ++sample) {
        PageSample *entry = &samples[sample];
        entry->page = (uint64_t)sample * page_count / sample_count;
        load_page(bitmap, bitmap_bytes, entry->page, entry->raw);
        if (all_zero(entry->raw)) {
            entry->mode = SAMPLE_IMPLICIT;
            continue;
        }
        size_t code_count = tunstall_encode(&dictionary, entry->raw, codes);
        if (code_count == SIZE_MAX)
            goto done;
        if (code_count * dictionary.code_bytes >= WDL_PAGE_SIZE) {
            entry->mode = SAMPLE_RAW;
        } else {
            entry->mode = SAMPLE_TUNSTALL;
            entry->code_count = code_count;
            entry->codes = malloc(code_count * dictionary.code_bytes);
            if (entry->codes == NULL)
                goto done;
            for (size_t code = 0; code < code_count; ++code) {
                entry->codes[code * dictionary.code_bytes] =
                    (unsigned char)codes[code];
                if (dictionary.code_bytes == 2)
                    entry->codes[code * 2 + 1] =
                        (unsigned char)(codes[code] >> 8);
            }
        }
        size_t compressed = ZSTD_compressCCtx(
            compressor, zstd_buffer, ZSTD_compressBound(WDL_PAGE_SIZE),
            entry->raw, WDL_PAGE_SIZE, 3);
        if (ZSTD_isError(compressed))
            goto done;
        entry->zstd_size = compressed;
        entry->zstd = malloc(compressed);
        if (entry->zstd == NULL)
            goto done;
        memcpy(entry->zstd, zstd_buffer, compressed);
    }

    unsigned char decoded[WDL_PAGE_SIZE];
    for (size_t sample = 0; sample < sample_count; ++sample) {
        PageSample *entry = &samples[sample];
        if (entry->mode == SAMPLE_IMPLICIT)
            memset(decoded, 0, sizeof(decoded));
        else if (entry->mode == SAMPLE_RAW)
            memcpy(decoded, entry->raw, sizeof(decoded));
        else if (!tunstall_decode(&dictionary, entry->codes,
                                  entry->code_count, decoded))
            goto done;
        if (memcmp(decoded, entry->raw, sizeof(decoded)) != 0)
            goto done;
        if (entry->mode != SAMPLE_IMPLICIT) {
            size_t expanded = ZSTD_decompressDCtx(
                decompressor, decoded, sizeof(decoded),
                entry->zstd, entry->zstd_size);
            if (ZSTD_isError(expanded) || expanded != sizeof(decoded) ||
                memcmp(decoded, entry->raw, sizeof(decoded)) != 0)
                goto done;
        }
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
            if (entry->mode == SAMPLE_IMPLICIT)
                memset(decoded, 0, sizeof(decoded));
            else if (entry->mode == SAMPLE_RAW)
                memcpy(decoded, entry->raw, sizeof(decoded));
            else if (!tunstall_decode(&dictionary, entry->codes,
                                      entry->code_count, decoded))
                goto done;
            checksum += decoded[(sample + round) & (WDL_PAGE_SIZE - 1)];
        }
    double tunstall_decode_time = now_seconds() - start;
    start = now_seconds();
    for (uint64_t round = 0; round < rounds; ++round)
        for (size_t sample = 0; sample < sample_count; ++sample) {
            PageSample *entry = &samples[sample];
            if (entry->mode == SAMPLE_IMPLICIT)
                memset(decoded, 0, sizeof(decoded));
            else {
                size_t expanded = ZSTD_decompressDCtx(
                    decompressor, decoded, sizeof(decoded),
                    entry->zstd, entry->zstd_size);
                if (ZSTD_isError(expanded) || expanded != sizeof(decoded))
                    goto done;
            }
            checksum += decoded[(sample + round) & (WDL_PAGE_SIZE - 1)];
        }
    double zstd_decode_time = now_seconds() - start;

    uint64_t dictionary_bytes =
        dictionary.leaf_count * sizeof(uint16_t) + dictionary.phrase_bytes;
    uint64_t directory_bytes = page_count * 14 + 64;
    uint64_t tunstall_file = tunstall_payload + directory_bytes +
                             dictionary_bytes;
    uint64_t zstd_file = zstd_payload + directory_bytes;
    printf("database: %s\n", name);
    printf("positions: %" PRIu64 " raw-packed: %.2f MiB pages: %" PRIu64
           " implicit-draw: %" PRIu64 "\n",
           maximum_index + 1, bitmap_bytes / 1048576.0,
           page_count, implicit_pages);
    printf("symbols:");
    for (unsigned symbol = 0; symbol < TUNSTALL_SYMBOLS; ++symbol)
        if (frequencies[symbol] != 0)
            printf(" %X=%.3f%%", symbol,
                   100.0 * frequencies[symbol] / (2.0 * bitmap_bytes));
    printf("\n");
    printf("Tunstall dictionary: %u-bit codes, %u entries, %.2f MiB "
           "serialized, built %.3f s\n",
           code_bits, dictionary.leaf_count,
           dictionary_bytes / 1048576.0, dictionary_time);
    printf("Tunstall: payload %.2f MiB, estimated file %.2f MiB, %.2f%% raw, "
           "raw-fallback pages=%" PRIu64 ", compression %.3f s\n",
           tunstall_payload / 1048576.0, tunstall_file / 1048576.0,
           100.0 * tunstall_file / bitmap_bytes, raw_tunstall_pages,
           tunstall_compress_time);
    printf("Zstd-3:  payload %.2f MiB, estimated file %.2f MiB, %.2f%% raw, "
           "compression %.3f s\n",
           zstd_payload / 1048576.0, zstd_file / 1048576.0,
           100.0 * zstd_file / bitmap_bytes, zstd_compress_time);
    printf("existing WDL file: %.2f MiB, %.2f%% raw\n",
           existing_file_bytes / 1048576.0,
           100.0 * existing_file_bytes / bitmap_bytes);
    uint64_t decoded_bytes = rounds * sample_count * WDL_PAGE_SIZE;
    printf("decode benchmark: %.2f MiB, %zu sampled pages, checksum=%" PRIu64
           "\n", decoded_bytes / 1048576.0, sample_count, checksum);
    printf("Tunstall decode: %.3f s, %.1f MiB/s\n",
           tunstall_decode_time,
           decoded_bytes / 1048576.0 / tunstall_decode_time);
    printf("Zstd-3 decode:  %.3f s, %.1f MiB/s\n",
           zstd_decode_time,
           decoded_bytes / 1048576.0 / zstd_decode_time);
    result = EXIT_SUCCESS;
done:
    free_samples(samples, sample_count);
    ZSTD_freeCCtx(compressor);
    ZSTD_freeDCtx(decompressor);
    free(zstd_buffer);
    tunstall_destroy(&dictionary);
    free(bitmap);
    return result;
}
