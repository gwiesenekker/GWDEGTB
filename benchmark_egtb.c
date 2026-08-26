#define _POSIX_C_SOURCE 200809L

#include "egtb.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_POSITIONS UINT64_C(4194304)
#define DEFAULT_PAGE_SIZE 1024
#define DEFAULT_CACHE_BYTES (UINT64_C(1) << 20)

static uint64_t random_state;

static uint64_t next_random(void)
{
    uint64_t value = random_state;
    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    random_state = value;
    return value * UINT64_C(2685821657736338717);
}

static double now_seconds(void)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (double)time.tv_sec + (double)time.tv_nsec * 1e-9;
}

static int16_t random_dtm(void)
{
    if ((next_random() & 1) != 0)
        return (int16_t)(1 + 2 * (next_random() % 256));
    return (int16_t)(-2 * (int)(next_random() % 257));
}

static bool selected(unsigned density)
{
    return next_random() % 100 < density;
}

static bool benchmark_density(unsigned density, uint64_t positions,
                              uint32_t page_size, uint64_t random_lookups,
                              uint64_t cache_bytes)
{
    char path[] = "/tmp/ipd-egtb-benchmark-XXXXXX";
    char direct_path[] = "/tmp/ipd-egtb-direct-benchmark-XXXXXX";
    EgtbCreateOptions options = {1024, 20, 3};
    EgtbCreateOptions direct_options = {1, 20, 3};
    EgtbStorageStatistics storage;
    EgtbCacheStatistics direct_cache;
    Egtb *egtb = NULL, *direct_egtb = NULL;
    EgtbView *view = NULL;
    uint64_t index, non_draw_values = 0, checked_non_draw_values = 0;
    uint64_t total_values = positions * 2;
    uint64_t lookup, lookup_checksum = 0, direct_lookup_checksum = 0;
    size_t read_cache_pages = (size_t)(cache_bytes / page_size);
    double write_start, write_seconds, direct_write_seconds;
    double compact_start, compact_seconds;
    double read_start, read_seconds, direct_read_seconds;
    double random_read_start, random_read_seconds, direct_random_read_seconds;
    int descriptor = mkstemp(path);
    int direct_descriptor = mkstemp(direct_path);

    if (descriptor < 0 || direct_descriptor < 0) {
        perror("mkstemp");
        if (descriptor >= 0)
            close(descriptor);
        if (direct_descriptor >= 0)
            close(direct_descriptor);
        return false;
    }
    close(descriptor);
    close(direct_descriptor);
    unlink(path);
    unlink(direct_path);

    random_state = UINT64_C(0xd1b54a32d192ed03);
    if (!egtb_create(&egtb, path, positions - 1, page_size, &options))
        goto failure;

    write_start = now_seconds();
    for (index = 0; index < positions; ++index) {
        unsigned side;
        for (side = 0; side < 2; ++side) {
            if (selected(density)) {
                if (!egtb_set(egtb, index, (EgtbSide)side, random_dtm()))
                    goto failure;
                ++non_draw_values;
            }
        }
    }
    if (!egtb_flush(egtb))
        goto failure;
    write_seconds = now_seconds() - write_start;
    if (!egtb_close(egtb)) {
        egtb = NULL;
        goto failure;
    }
    egtb = NULL;

    random_state = UINT64_C(0xd1b54a32d192ed03);
    if (!egtb_create(&direct_egtb, direct_path, positions - 1, page_size,
                     &direct_options) ||
        !egtb_view_create(&view, direct_egtb, options.cache_pages, true))
        goto failure;
    write_start = now_seconds();
    for (index = 0; index < positions; ++index) {
        for (unsigned side = 0; side < 2; ++side) {
            if (selected(density) &&
                !egtb_view_set(view, index, (EgtbSide)side, random_dtm()))
                goto failure;
        }
    }
    if (!egtb_view_flush(view))
        goto failure;
    direct_write_seconds = now_seconds() - write_start;
    if (!egtb_view_close(view)) {
        view = NULL;
        goto failure;
    }
    view = NULL;
    if (!egtb_close(direct_egtb)) {
        direct_egtb = NULL;
        goto failure;
    }
    direct_egtb = NULL;
    unlink(direct_path);

    compact_start = now_seconds();
    if (!egtb_compact(path, 9, 64))
        goto failure;
    compact_seconds = now_seconds() - compact_start;

    if (read_cache_pages == 0)
        read_cache_pages = 1;
    if (!egtb_open_readonly(&egtb, path, read_cache_pages))
        goto failure;
    if (!egtb_storage_statistics(egtb, &storage))
        goto failure;
    read_start = now_seconds();
    for (index = 0; index < positions; ++index) {
        unsigned side;
        for (side = 0; side < 2; ++side) {
            int16_t value;
            if (!egtb_get(egtb, index, (EgtbSide)side, &value))
                goto failure;
            if (value != EGTB_DRAW)
                ++checked_non_draw_values;
        }
    }
    read_seconds = now_seconds() - read_start;
    if (checked_non_draw_values != non_draw_values) {
        fprintf(stderr, "density %u%%: read-back count mismatch\n", density);
        goto failure;
    }

    random_state = UINT64_C(0x8f3f73b5cf1c9ade);
    random_read_start = now_seconds();
    for (lookup = 0; lookup < random_lookups; ++lookup) {
        int16_t value;
        index = next_random() % positions;
        if (!egtb_get(egtb, index, (EgtbSide)(next_random() & 1), &value))
            goto failure;
        lookup_checksum += (uint16_t)value;
    }
    random_read_seconds = now_seconds() - random_read_start;
    if (!egtb_close(egtb)) {
        egtb = NULL;
        goto failure;
    }
    egtb = NULL;

    if (!egtb_open_readonly(&direct_egtb, path, 1) ||
        !egtb_view_create(&view, direct_egtb, read_cache_pages, false))
        goto failure;
    checked_non_draw_values = 0;
    read_start = now_seconds();
    for (index = 0; index < positions; ++index) {
        for (unsigned side = 0; side < 2; ++side) {
            int16_t value;
            if (!egtb_view_get(view, index, (EgtbSide)side, &value))
                goto failure;
            if (value != EGTB_DRAW)
                ++checked_non_draw_values;
        }
    }
    direct_read_seconds = now_seconds() - read_start;
    if (checked_non_draw_values != non_draw_values)
        goto failure;
    random_state = UINT64_C(0x8f3f73b5cf1c9ade);
    random_read_start = now_seconds();
    for (lookup = 0; lookup < random_lookups; ++lookup) {
        int16_t value;
        index = next_random() % positions;
        if (!egtb_view_get(view, index, (EgtbSide)(next_random() & 1),
                           &value))
            goto failure;
        direct_lookup_checksum += (uint16_t)value;
    }
    direct_random_read_seconds = now_seconds() - random_read_start;
    egtb_view_cache_statistics(view, &direct_cache);
    if (direct_lookup_checksum != lookup_checksum ||
        !egtb_view_close(view)) {
        view = NULL;
        goto failure;
    }
    view = NULL;
    if (!egtb_close(direct_egtb)) {
        direct_egtb = NULL;
        goto failure;
    }
    direct_egtb = NULL;

    printf("%3u%% actual=%6.2f%%  write handle/view=%7.2f/%7.2f Mvalues/s\n"
           "     seq-read handle/view=%7.2f/%7.2f Mvalues/s  "
           "random handle/view=%7.3f/%7.3f Mlookups/s  hit=%5.1f%%  "
           "compact=%6.3f s  "
           "payload=%6.2f%%  overall=%6.2f%%  file=%7.2f MiB\n",
           density, 100.0 * (double)non_draw_values / (double)total_values,
           (double)total_values / write_seconds / 1e6,
           (double)total_values / direct_write_seconds / 1e6,
           (double)total_values / read_seconds / 1e6,
           (double)total_values / direct_read_seconds / 1e6,
           (double)random_lookups / random_read_seconds / 1e6,
           (double)random_lookups / direct_random_read_seconds / 1e6,
           direct_cache.lookups == 0 ? 0.0 :
               100.0 * (double)direct_cache.hits /
                   (double)direct_cache.lookups,
           compact_seconds,
           100.0 * (double)storage.compressed_payload_bytes /
               (double)storage.logical_uncompressed_bytes,
           100.0 * (double)storage.file_bytes /
               (double)storage.logical_uncompressed_bytes,
           (double)storage.file_bytes / (1024.0 * 1024.0));
    if (lookup_checksum == UINT64_MAX)
        fprintf(stderr, "lookup checksum=%" PRIu64 "\n", lookup_checksum);
    unlink(path);
    unlink(direct_path);
    return true;

failure:
    fprintf(stderr, "density %u%% failed: %s\n", density, egtb_last_error());
    if (egtb != NULL)
        egtb_close(egtb);
    if (view != NULL)
        egtb_view_close(view);
    if (direct_egtb != NULL)
        egtb_close(direct_egtb);
    unlink(path);
    unlink(direct_path);
    return false;
}

int main(int argc, char **argv)
{
    static const unsigned densities[] = {10, 50, 90};
    uint64_t positions = DEFAULT_POSITIONS;
    uint64_t random_lookups = DEFAULT_POSITIONS;
    uint64_t cache_bytes = DEFAULT_CACHE_BYTES;
    uint32_t page_size = DEFAULT_PAGE_SIZE;
    size_t i;

    for (i = 1; i < (size_t)argc; i += 2) {
        char *end;
        unsigned long long value;
        if (i + 1 >= (size_t)argc) {
            fprintf(stderr, "missing value for %s\n", argv[i]);
            return EXIT_FAILURE;
        }
        errno = 0;
        value = strtoull(argv[i + 1], &end, 10);
        if (errno != 0 || *argv[i + 1] == '\0' || *end != '\0' || value == 0) {
            fprintf(stderr, "invalid value: %s\n", argv[i + 1]);
            return EXIT_FAILURE;
        }
        if (strcmp(argv[i], "--positions") == 0 && value <= UINT64_MAX / 2) {
            positions = (uint64_t)value;
        } else if (strcmp(argv[i], "--lookups") == 0) {
            random_lookups = (uint64_t)value;
        } else if (strcmp(argv[i], "--cache-mib") == 0 &&
                   value <= UINT64_MAX / (UINT64_C(1) << 20)) {
            cache_bytes = (uint64_t)value << 20;
        } else if (strcmp(argv[i], "--page-size") == 0 &&
                   value <= UINT32_MAX && value % sizeof(EgtbEntry) == 0) {
            page_size = (uint32_t)value;
        } else {
            fprintf(stderr, "invalid option or value: %s %s\n",
                    argv[i], argv[i + 1]);
            return EXIT_FAILURE;
        }
    }

    printf("EGTB density benchmark: positions=%" PRIu64
           " values=%" PRIu64 " page=%u bytes cache=%.2f MiB "
           "random-lookups=%" PRIu64 " reserve=20%%\n",
           positions, positions * 2, page_size,
           (double)cache_bytes / (1024.0 * 1024.0), random_lookups);
    for (i = 0; i < sizeof(densities) / sizeof(densities[0]); ++i) {
        if (!benchmark_density(densities[i], positions, page_size,
                               random_lookups, cache_bytes))
            return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
