#include "combinatorial_index.h"
#include "endgame_index.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    unsigned wm, bm, wk, bk;
} Material;

static const Material materials[] = {
    {0, 0, 1, 1},
    {1, 0, 1, 1},
    {1, 1, 1, 1},
    {1, 1, 2, 1},
    {1, 1, 2, 2},
    {1, 2, 2, 2}
};

static bool same_position(const EgPosition *left, const EgPosition *right)
{
    return left->white_men == right->white_men &&
           left->black_men == right->black_men &&
           left->white_kings == right->white_kings &&
           left->black_kings == right->black_kings;
}

static bool test_material(const Material *material)
{
    EgIndexer legacy;
    CombinatorialIndexer combinatorial;
    uint64_t positions, samples, sample;
    if (!eg_indexer_init(&legacy, material->wm, material->bm,
                         material->wk, material->bk) ||
        !comb_indexer_init(&combinatorial, material->wm, material->bm,
                           material->wk, material->bk)) {
        fprintf(stderr, "indexer initialization failed\n");
        return false;
    }
    positions = eg_position_count(&legacy);
    if (positions != comb_position_count(&combinatorial)) {
        fprintf(stderr, "position-count mismatch: legacy=%" PRIu64
                        " combinatorial=%" PRIu64 "\n",
                positions, comb_position_count(&combinatorial));
        return false;
    }
    samples = positions < UINT64_C(5000000) ? positions : UINT64_C(1000000);
    for (sample = 0; sample < samples; ++sample) {
        uint64_t legacy_index =
            (uint64_t)(((__uint128_t)sample * positions) / samples);
        uint64_t combinatorial_index = 0, reranked;
        EgPosition position, decoded;
        if (!eg_index_to_position(&legacy, legacy_index, &position) ||
            !comb_position_to_index(&combinatorial, &position,
                                    &combinatorial_index) ||
            combinatorial_index >= positions ||
            !comb_index_to_position(&combinatorial, combinatorial_index,
                                    &decoded) ||
            !same_position(&position, &decoded) ||
            !comb_position_to_index(&combinatorial, &decoded, &reranked) ||
            reranked != combinatorial_index) {
            fprintf(stderr, "round trip failed at legacy index %" PRIu64
                            " combinatorial index %" PRIu64 "\n",
                    legacy_index, combinatorial_index);
            return false;
        }
    }
    printf("WM=%u BM=%u WK=%u BK=%u positions=%" PRIu64
           " distributions=%zu samples=%" PRIu64 ": PASS\n",
           material->wm, material->bm, material->wk, material->bk,
           positions, combinatorial.distribution_count, samples);
    eg_indexer_destroy(&legacy);
    comb_indexer_destroy(&combinatorial);
    return true;
}

int main(void)
{
    size_t material;
    for (material = 0;
         material < sizeof(materials) / sizeof(materials[0]); ++material)
        if (!test_material(&materials[material]))
            return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
