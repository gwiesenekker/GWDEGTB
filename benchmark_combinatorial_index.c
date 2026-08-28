#define _POSIX_C_SOURCE 200809L

#include "combinatorial_index.h"
#include "endgame_index.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define SAMPLE_COUNT UINT64_C(1000000)
#define MIN_RANK_OPERATIONS UINT64_C(100000000)
#define MIN_INVERSE_OPERATIONS UINT64_C(20000000)

typedef struct {
    unsigned pieces;
    unsigned wm, bm, wk, bk;
} Material;

static const Material materials[] = {
    {2, 0, 0, 1, 1},
    {3, 1, 0, 1, 1},
    {4, 1, 1, 1, 1},
    {5, 1, 1, 2, 1},
    {6, 1, 1, 2, 2},
    {7, 1, 2, 2, 2}
};

static double now_seconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (double)now.tv_sec + (double)now.tv_nsec * 1e-9;
}

static uint64_t position_hash(const EgPosition *position)
{
    return position->white_men ^ (position->black_men << 1) ^
           (position->white_kings << 2) ^ (position->black_kings << 3);
}

static uint64_t scaled_index(uint64_t sample, uint64_t count,
                             uint64_t samples)
{
    return (uint64_t)(((__uint128_t)sample * count) / samples);
}

static bool benchmark_material(const Material *material)
{
    EgIndexer transition;
    CombinatorialIndexer combinatorial;
    EgPosition *positions;
    uint64_t count, samples, sample, pass, passes, operations;
    uint64_t rank_operations;
    uint64_t checksum, transition_checksum, combinatorial_checksum;
    double start, transition_rank_seconds, combinatorial_rank_seconds;
    double transition_inverse_seconds, combinatorial_inverse_seconds;

    if (!eg_indexer_init(&transition, material->wm, material->bm,
                         material->wk, material->bk) ||
        !comb_indexer_init(&combinatorial, material->wm, material->bm,
                           material->wk, material->bk))
        return false;
    count = eg_position_count(&transition);
    if (count != comb_position_count(&combinatorial))
        return false;
    samples = count < SAMPLE_COUNT ? count : SAMPLE_COUNT;
    positions = malloc((size_t)samples * sizeof(*positions));
    if (positions == NULL)
        return false;
    for (sample = 0; sample < samples; ++sample) {
        uint64_t index = scaled_index(sample, count, samples);
        uint64_t ranked;
        if (!eg_index_to_position(&transition, index, &positions[sample]) ||
            !eg_position_to_index(&transition, &positions[sample], &ranked) ||
            ranked != index ||
            !comb_position_to_index(&combinatorial, &positions[sample],
                                    &ranked))
            return false;
    }

    passes = (MIN_RANK_OPERATIONS + samples - 1) / samples;
    rank_operations = passes * samples;
    transition_checksum = 0;
    start = now_seconds();
    for (pass = 0; pass < passes; ++pass)
        for (sample = 0; sample < samples; ++sample) {
            uint64_t index;
            if (!eg_position_to_index(&transition, &positions[sample], &index))
                return false;
            transition_checksum += index;
        }
    transition_rank_seconds = now_seconds() - start;

    combinatorial_checksum = 0;
    start = now_seconds();
    for (pass = 0; pass < passes; ++pass)
        for (sample = 0; sample < samples; ++sample) {
            uint64_t index;
            if (!comb_position_to_index(&combinatorial, &positions[sample],
                                        &index))
                return false;
            combinatorial_checksum += index;
        }
    combinatorial_rank_seconds = now_seconds() - start;

    passes = (MIN_INVERSE_OPERATIONS + samples - 1) / samples;
    operations = passes * samples;
    checksum = 0;
    start = now_seconds();
    for (pass = 0; pass < passes; ++pass)
        for (sample = 0; sample < samples; ++sample) {
            EgPosition position;
            uint64_t index = scaled_index(sample, count, samples);
            if (!eg_index_to_position(&transition, index, &position))
                return false;
            checksum += position_hash(&position);
        }
    transition_inverse_seconds = now_seconds() - start;

    checksum ^= transition_checksum ^ combinatorial_checksum;
    start = now_seconds();
    for (pass = 0; pass < passes; ++pass)
        for (sample = 0; sample < samples; ++sample) {
            EgPosition position;
            uint64_t index = scaled_index(sample, count, samples);
            if (!comb_index_to_position(&combinatorial, index, &position))
                return false;
            checksum += position_hash(&position);
        }
    combinatorial_inverse_seconds = now_seconds() - start;

    printf("%u-piece WM=%u BM=%u WK=%u BK=%u positions=%" PRIu64
           " distributions=%zu\n",
           material->pieces, material->wm, material->bm, material->wk,
           material->bk, count, combinatorial.distribution_count);
    printf("  transition rank:    %12.3f positions/s\n",
           (double)rank_operations / transition_rank_seconds);
    printf("  combinatorial rank: %12.3f positions/s  speedup %.3fx\n",
           (double)rank_operations / combinatorial_rank_seconds,
           transition_rank_seconds / combinatorial_rank_seconds);
    printf("  transition inverse: %12.3f positions/s\n",
           (double)operations / transition_inverse_seconds);
    printf("  combinatorial inv.: %12.3f positions/s  speedup %.3fx\n",
           (double)operations / combinatorial_inverse_seconds,
           transition_inverse_seconds / combinatorial_inverse_seconds);
    printf("  checksum: %" PRIu64 "\n\n", checksum);

    free(positions);
    eg_indexer_destroy(&transition);
    comb_indexer_destroy(&combinatorial);
    return true;
}

int main(void)
{
    size_t material;
    for (material = 0;
         material < sizeof(materials) / sizeof(materials[0]); ++material)
        if (!benchmark_material(&materials[material]))
            return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
