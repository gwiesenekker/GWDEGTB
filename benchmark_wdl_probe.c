#define _POSIX_C_SOURCE 200809L

#include "endgame_index.h"
#include "gwdegtb.h"
#include "material.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    uint64_t wk, wm, bk, bm;
    GwdegtbSide side;
} Query;

static double seconds(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec + now.tv_nsec * 1e-9;
}

static uint64_t compact_to_gwd(uint64_t compact)
{
    uint64_t padded = 0;
    for (unsigned square = 0; square < 50; ++square)
        if ((compact & (UINT64_C(1) << square)) != 0)
            padded |= UINT64_C(1) << (square + 6 + square / 10);
    return padded;
}

static uint64_t random64(uint64_t *state)
{
    uint64_t value = *state;
    value ^= value << 13;
    value ^= value >> 7;
    value ^= value << 17;
    return *state = value;
}

static bool parse_material(const char *name, EgtbMaterial *canonical)
{
    EgtbMaterial requested;
    int consumed = 0;
    if (sscanf(name, "%uwX-%uwO-%ubX-%ubO%n",
               &requested.white_kings, &requested.white_men,
               &requested.black_kings, &requested.black_men,
               &consumed) != 4 ||
        (name[consumed] != '\0' && strcmp(name + consumed, ".wdl") != 0))
        return false;
    EgtbMaterialKind kind = egtb_material_resolve(&requested, canonical);
    return kind == EGTB_MATERIAL_CANONICAL ||
           kind == EGTB_MATERIAL_MIRROR;
}

int main(int argc, char **argv)
{
    const char *directory, *name;
    EgtbMaterial material;
    EgIndexer indexer;
    unsigned char *resident = NULL, *compressed = NULL;
    GwdegtbWdlProbe *probe = NULL;
    Query *queries = NULL;
    uint64_t maximum_index, query_count = UINT64_C(1000000);
    size_t resident_bytes, compressed_bytes, cache_bytes;
    uint64_t rng = UINT64_C(0x6a09e667f3bcc909), checksum = 0;
    double start, resident_time, compressed_time;
    int result = EXIT_FAILURE;

    if (argc < 3 || argc > 5) {
        fprintf(stderr, "usage: %s DIRECTORY DATABASE [CACHE_MIB] [LOOKUPS]\n",
                argv[0]);
        return EXIT_FAILURE;
    }
    directory = argv[1];
    name = argv[2];
    uint64_t cache_mib = argc >= 4 ? strtoull(argv[3], NULL, 10) : 64;
    cache_bytes = cache_mib <= SIZE_MAX / (UINT64_C(1) << 20)
                      ? (size_t)(cache_mib << 20) : 0;
    if (argc == 5)
        query_count = strtoull(argv[4], NULL, 10);
    if (query_count == 0 || query_count > SIZE_MAX / sizeof(*queries) ||
        cache_bytes == 0 ||
        !parse_material(name, &material) ||
        !eg_indexer_init(&indexer, material.white_men, material.black_men,
                         material.white_kings, material.black_kings)) {
        fprintf(stderr, "invalid material, cache size, or lookup count\n");
        return EXIT_FAILURE;
    }
    if (!gwdegtb_wdl_info(name, &maximum_index, &resident_bytes) ||
        !gwdegtb_wdl_compressed_info(directory, name, &compressed_bytes)) {
        fprintf(stderr, "cannot inspect WDL: %s\n", gwdegtb_last_error());
        goto done;
    }
    resident = malloc(resident_bytes);
    compressed = malloc(compressed_bytes);
    queries = malloc((size_t)query_count * sizeof(*queries));
    if (resident == NULL || compressed == NULL || queries == NULL ||
        !gwdegtb_wdl_decompress_threads(directory, name, resident,
                                        resident_bytes, 4) ||
        !gwdegtb_wdl_attach(name, resident, resident_bytes) ||
        !gwdegtb_wdl_compressed_load(directory, name, compressed,
                                     compressed_bytes) ||
        !gwdegtb_wdl_compressed_attach(name, compressed, compressed_bytes) ||
        !gwdegtb_wdl_probe_create(cache_bytes, &probe)) {
        fprintf(stderr, "cannot prepare WDL benchmark: %s\n",
                gwdegtb_last_error());
        goto done;
    }
    for (uint64_t query = 0; query < query_count; ++query) {
        EgPosition position;
        uint64_t index = random64(&rng) % (maximum_index + 1);
        if (!eg_index_to_position(&indexer, index, &position)) {
            fprintf(stderr, "cannot invert index %" PRIu64 "\n", index);
            goto done;
        }
        queries[query].wk = compact_to_gwd(position.white_kings);
        queries[query].wm = compact_to_gwd(position.white_men);
        queries[query].bk = compact_to_gwd(position.black_kings);
        queries[query].bm = compact_to_gwd(position.black_men);
        queries[query].side = (GwdegtbSide)(random64(&rng) & 1);
    }
    start = seconds();
    for (uint64_t query = 0; query < query_count; ++query)
        checksum += (uint16_t)gwdegtb_wdl_lookup(
            queries[query].wk, queries[query].wm,
            queries[query].bk, queries[query].bm, queries[query].side);
    resident_time = seconds() - start;
    start = seconds();
    for (uint64_t query = 0; query < query_count; ++query)
        checksum += (uint16_t)gwdegtb_wdl_lookup_probe(
            probe, queries[query].wk, queries[query].wm,
            queries[query].bk, queries[query].bm, queries[query].side);
    compressed_time = seconds() - start;
    GwdegtbWdlProbeStatistics statistics;
    gwdegtb_wdl_probe_statistics(probe, &statistics);
    printf("database: %s\n", name);
    printf("resident bitmap: %.2f MiB\n", resident_bytes / 1048576.0);
    printf("compressed image: %.2f MiB\n", compressed_bytes / 1048576.0);
    printf("probe cache requested: %.2f MiB per thread\n",
           statistics.requested_cache_bytes / 1048576.0);
    printf("probe cache allocated: %.2f MiB per thread (%" PRIu64
           " entries)\n",
           statistics.allocated_cache_bytes / 1048576.0,
           statistics.cache_entries);
    printf("random lookups: %" PRIu64 "\n", query_count);
    printf("resident:   %.3f s, %.0f lookups/s\n", resident_time,
           query_count / resident_time);
    printf("compressed: %.3f s, %.0f lookups/s, %.2fx resident time\n",
           compressed_time, query_count / compressed_time,
           compressed_time / resident_time);
    printf("compressed cache: hits=%" PRIu64 " misses=%" PRIu64
           " hit-rate=%.2f%% decompressions=%" PRIu64 "\n",
           statistics.hits, statistics.misses,
           statistics.lookups == 0 ? 0.0
               : 100.0 * statistics.hits / statistics.lookups,
           statistics.decompressions);
    printf("checksum: %" PRIu64 "\n", checksum);
    result = EXIT_SUCCESS;
done:
    gwdegtb_wdl_probe_destroy(probe);
    gwdegtb_wdl_compressed_unload_all();
    gwdegtb_wdl_unload_all();
    free(queries);
    free(compressed);
    free(resident);
    eg_indexer_destroy(&indexer);
    return result;
}
