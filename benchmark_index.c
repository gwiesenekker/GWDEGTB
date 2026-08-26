#define _POSIX_C_SOURCE 200809L

#include "endgame_index.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_SAMPLE_LIMIT UINT64_C(110000000)
#define MIN_OPERATIONS UINT64_C(100000000)
#define SAMPLE_BLOCKS 1024

typedef struct {
    unsigned pieces;
    unsigned wm, bm, wk, bk;
} Material;

typedef struct {
    uint64_t passes;
    uint64_t per_pass;
    uint64_t operations;
    unsigned blocks;
    bool sampled;
} Traversal;

static const Material largest[] = {
    {2, 0, 0, 1, 1},
    {3, 1, 0, 1, 1},
    {4, 1, 1, 1, 1},
    {5, 1, 1, 2, 1},
    {6, 1, 1, 2, 2},
    {7, 1, 2, 2, 2}
};

static double now_seconds(void)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (double)time.tv_sec + (double)time.tv_nsec / 1000000000.0;
}

static uint64_t position_hash(const EgPosition *position)
{
    return position->white_men ^
           (position->black_men << 1) ^
           (position->white_kings << 2) ^
           (position->black_kings << 3);
}

static Traversal make_traversal(uint64_t count, uint64_t sample_limit,
                                bool full)
{
    Traversal traversal = {1, count, count, 1, false};

    if (full)
        return traversal;
    if (count > sample_limit) {
        traversal.per_pass = sample_limit;
        traversal.operations = sample_limit;
        traversal.blocks = SAMPLE_BLOCKS;
        traversal.sampled = true;
    } else if (count < MIN_OPERATIONS) {
        traversal.passes = (MIN_OPERATIONS + count - 1) / count;
        traversal.operations = traversal.passes * count;
    }
    return traversal;
}

static uint64_t block_start(uint64_t count, const Traversal *traversal,
                            unsigned block)
{
    if (!traversal->sampled)
        return 0;
    return ((uint64_t)block * count) / traversal->blocks;
}

static uint64_t block_length(const Traversal *traversal, unsigned block)
{
    uint64_t length = traversal->per_pass / traversal->blocks;
    if (block < traversal->per_pass % traversal->blocks)
        ++length;
    return length;
}

static bool benchmark_material(const Material *material, uint64_t sample_limit,
                               bool full)
{
    EgIndexer indexer;
    EgPosition position;
    Traversal traversal;
    uint64_t count, pass, offset, i, ranked;
    uint64_t inverse_checksum = 0;
    uint64_t roundtrip_checksum = 0;
    unsigned block;
    double start, inverse_seconds, roundtrip_seconds, index_seconds;

    if (!eg_indexer_init(&indexer, material->wm, material->bm,
                         material->wk, material->bk)) {
        fprintf(stderr, "could not initialize %u-piece benchmark indexer\n",
                material->pieces);
        return false;
    }
    count = eg_position_count(&indexer);
    traversal = make_traversal(count, sample_limit, full);

    for (i = 0; i < count && i < UINT64_C(100000); ++i) {
        if (!eg_index_to_position(&indexer, i, &position) ||
            !eg_position_to_index(&indexer, &position, &ranked) || ranked != i) {
            fprintf(stderr, "warm-up round trip failed at index %" PRIu64 "\n", i);
            eg_indexer_destroy(&indexer);
            return false;
        }
    }

    start = now_seconds();
    for (pass = 0; pass < traversal.passes; ++pass)
    for (block = 0; block < traversal.blocks; ++block) {
        uint64_t first = block_start(count, &traversal, block);
        uint64_t length = block_length(&traversal, block);
        for (offset = 0; offset < length; ++offset) {
            i = first + offset;
            if (!eg_index_to_position(&indexer, i, &position)) {
                fprintf(stderr, "inverse failed at index %" PRIu64 "\n", i);
                eg_indexer_destroy(&indexer);
                return false;
            }
            inverse_checksum += position_hash(&position);
        }
    }
    inverse_seconds = now_seconds() - start;

    start = now_seconds();
    for (pass = 0; pass < traversal.passes; ++pass)
    for (block = 0; block < traversal.blocks; ++block) {
        uint64_t first = block_start(count, &traversal, block);
        uint64_t length = block_length(&traversal, block);
        for (offset = 0; offset < length; ++offset) {
            i = first + offset;
            if (!eg_index_to_position(&indexer, i, &position) ||
                !eg_position_to_index(&indexer, &position, &ranked) ||
                ranked != i) {
                fprintf(stderr, "round trip failed at index %" PRIu64 "\n", i);
                eg_indexer_destroy(&indexer);
                return false;
            }
            roundtrip_checksum += position_hash(&position);
        }
    }
    roundtrip_seconds = now_seconds() - start;
    index_seconds = roundtrip_seconds - inverse_seconds;

    printf("%u-piece  WM=%u BM=%u WK=%u BK=%u\n", material->pieces,
           material->wm, material->bm, material->wk, material->bk);
    printf("  positions:  %" PRIu64 " (max index %" PRIu64 ")\n", count,
           count - 1);
    if (traversal.sampled)
        printf("  coverage:   %" PRIu64 " indices in %u evenly spaced blocks\n",
               traversal.operations, traversal.blocks);
    else if (traversal.passes > 1)
        printf("  coverage:   full database repeated %" PRIu64 " times\n",
               traversal.passes);
    else
        printf("  coverage:   full database\n");
    printf("  inverse:    %10.3f inversions/s  (%.6f s)\n",
           (double)traversal.operations / inverse_seconds, inverse_seconds);
    printf("  round trip: %10.3f round trips/s (%.6f s)\n",
           (double)traversal.operations / roundtrip_seconds, roundtrip_seconds);
    if (index_seconds > 0.0)
        printf("  index:      %10.3f indices/s     (%.6f s by delta)\n",
               (double)traversal.operations / index_seconds, index_seconds);
    else
        printf("  index:      non-positive timing delta; rerun benchmark\n");
    printf("  checksum:   %s\n\n",
           inverse_checksum == roundtrip_checksum ? "match" : "MISMATCH");

    eg_indexer_destroy(&indexer);
    return inverse_checksum == roundtrip_checksum && index_seconds > 0.0;
}

static bool parse_limit(const char *text, uint64_t *limit)
{
    char *end;
    uintmax_t parsed;
    errno = 0;
    parsed = strtoumax(text, &end, 10);
    if (errno != 0 || *text == '\0' || *end != '\0' || parsed == 0 ||
        parsed > UINT64_MAX)
        return false;
    *limit = (uint64_t)parsed;
    return true;
}

int main(int argc, char **argv)
{
    uint64_t sample_limit = DEFAULT_SAMPLE_LIMIT;
    bool full = false;
    size_t i;

    if (argc == 2 && strcmp(argv[1], "--full") == 0) {
        full = true;
    } else if (argc == 3 && strcmp(argv[1], "--samples") == 0) {
        if (!parse_limit(argv[2], &sample_limit)) {
            fprintf(stderr, "invalid sample limit: %s\n", argv[2]);
            return EXIT_FAILURE;
        }
    } else if (argc != 1) {
        fprintf(stderr, "usage: %s [--full | --samples COUNT]\n", argv[0]);
        return EXIT_FAILURE;
    }

    for (i = 0; i < sizeof(largest) / sizeof(largest[0]); ++i) {
        if (!benchmark_material(&largest[i], sample_limit, full))
            return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
