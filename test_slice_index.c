#include "endgame_index.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    unsigned wm, bm, wk, bk;
} Material;

static bool same_position(const EgPosition *a, const EgPosition *b)
{
    return a->white_men == b->white_men &&
           a->black_men == b->black_men &&
           a->white_kings == b->white_kings &&
           a->black_kings == b->black_kings;
}

static bool test_material(Material material)
{
    EgIndexer full = {0};
    EgIndexer slices[10][9];
    uint64_t observed[10][9] = {{0}};
    uint64_t total = 0;
    int first_white = material.wm == 0 ? 0 : 1;
    int last_white = material.wm == 0 ? 0 : 9;
    int first_black = 0;
    int last_black = material.bm == 0 ? 0 : 8;
    bool ok = false;
    memset(slices, 0, sizeof(slices));
    if (!eg_indexer_init(&full, material.wm, material.bm,
                         material.wk, material.bk))
        goto done;
    for (int wr = first_white; wr <= last_white; ++wr)
        for (int br = first_black; br <= last_black; ++br)
            if (!eg_slice_indexer_init(
                    &slices[wr][br], material.wm, material.bm,
                    material.wk, material.bk,
                    material.wm == 0 ? -1 : wr,
                    material.bm == 0 ? -1 : br))
                goto done;
    for (uint64_t full_index = 0;
         full_index < eg_position_count(&full); ++full_index) {
        EgPosition position, decoded;
        uint64_t local_index, ranked_full;
        int wr, br;
        if (!eg_index_to_position(&full, full_index, &position))
            goto done;
        eg_position_slice(&position, &wr, &br);
        if (wr < 0)
            wr = 0;
        if (br < 0)
            br = 0;
        if (!eg_indexer_contains_position(&slices[wr][br], &position) ||
            !eg_position_to_index(&slices[wr][br], &position, &local_index) ||
            local_index != observed[wr][br]++ ||
            !eg_index_to_position(&slices[wr][br], local_index, &decoded) ||
            !same_position(&position, &decoded) ||
            !eg_position_to_index(&full, &decoded, &ranked_full) ||
            ranked_full != full_index)
            goto done;
    }
    for (int wr = first_white; wr <= last_white; ++wr)
        for (int br = first_black; br <= last_black; ++br) {
            if (observed[wr][br] != eg_position_count(&slices[wr][br]))
                goto done;
            total += observed[wr][br];
        }
    if (total != eg_position_count(&full))
        goto done;
    printf("slice index: WM=%u BM=%u WK=%u BK=%u positions=%" PRIu64
           " PASS\n", material.wm, material.bm, material.wk, material.bk,
           total);
    ok = true;
done:
    for (int wr = first_white; wr <= last_white; ++wr)
        for (int br = first_black; br <= last_black; ++br)
            eg_indexer_destroy(&slices[wr][br]);
    eg_indexer_destroy(&full);
    return ok;
}

int main(void)
{
    static const Material materials[] = {
        {0, 0, 1, 1},
        {1, 0, 0, 1},
        {0, 1, 1, 0},
        {1, 1, 0, 0},
        {2, 1, 0, 0},
        {1, 1, 1, 0}
    };
    for (size_t i = 0; i < sizeof(materials) / sizeof(materials[0]); ++i)
        if (!test_material(materials[i])) {
            fprintf(stderr, "slice index test failed for material %zu\n", i);
            return 1;
        }
    return 0;
}
