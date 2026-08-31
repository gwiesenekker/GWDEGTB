#include "material.h"

#include <stdio.h>
#include <stdlib.h>

#define BIT(square) (UINT64_C(1) << (square))

static bool same_material(const EgtbMaterial *a, const EgtbMaterial *b)
{
    return a->white_kings == b->white_kings &&
           a->white_men == b->white_men &&
           a->black_kings == b->black_kings &&
           a->black_men == b->black_men;
}

int main(void)
{
    EgtbMaterial wk_bm = {1, 0, 0, 1};
    EgtbMaterial wm_bk = {0, 1, 1, 0};
    EgtbMaterial wk_bk = {1, 0, 1, 0};
    EgtbMaterial wm_bm = {0, 1, 0, 1};
    EgtbMaterial canonical;
    DraughtsPosition position = {0}, mirrored, restored;
    uint64_t canonical_count = 0, mirror_count = 0;
    unsigned wk, wm, bk, bm;
    if (egtb_material_resolve(&wk_bk, &canonical) !=
            EGTB_MATERIAL_CANONICAL || !same_material(&wk_bk, &canonical) ||
        egtb_material_resolve(&wk_bm, &canonical) !=
            EGTB_MATERIAL_CANONICAL || !same_material(&wk_bm, &canonical) ||
        egtb_material_resolve(&wm_bm, &canonical) !=
            EGTB_MATERIAL_CANONICAL || !same_material(&wm_bm, &canonical) ||
        egtb_material_resolve(&wm_bk, &canonical) != EGTB_MATERIAL_MIRROR ||
        !same_material(&wk_bm, &canonical)) {
        fprintf(stderr, "GWD two-piece canonicalization failed\n");
        return EXIT_FAILURE;
    }
    for (wk = 0; wk <= EGTB_MAX_PIECES; ++wk)
        for (wm = 0; wm <= EGTB_MAX_PIECES; ++wm)
            for (bk = 0; bk <= EGTB_MAX_PIECES; ++bk)
                for (bm = 0; bm <= EGTB_MAX_PIECES; ++bm) {
                    EgtbMaterial material = {wk, wm, bk, bm};
                    unsigned total = wk + wm + bk + bm;
                    EgtbMaterialKind kind;
                    if (total < 2 || total > EGTB_MAX_PIECES ||
                        wk + wm == 0 || bk + bm == 0)
                        continue;
                    kind = egtb_material_resolve(&material, &canonical);
                    if (kind == EGTB_MATERIAL_CANONICAL)
                        ++canonical_count;
                    else if (kind == EGTB_MATERIAL_MIRROR)
                        ++mirror_count;
                    else
                        return EXIT_FAILURE;
                }
    if (canonical_count != 210 || mirror_count != 196) {
        fprintf(stderr, "unexpected material catalog counts\n");
        return EXIT_FAILURE;
    }
    position.white_men = BIT(5);
    position.black_men = BIT(44);
    position.white_kings = BIT(0) | BIT(17);
    position.black_kings = BIT(49) | BIT(32);
    egtb_mirror_position(&position, &mirrored);
    egtb_mirror_position(&mirrored, &restored);
    if (restored.white_men != position.white_men ||
        restored.black_men != position.black_men ||
        restored.white_kings != position.white_kings ||
        restored.black_kings != position.black_kings ||
        egtb_mirror_side(egtb_mirror_side(EGTB_WHITE_TO_MOVE)) !=
            EGTB_WHITE_TO_MOVE) {
        fprintf(stderr, "material mirror round trip failed\n");
        return EXIT_FAILURE;
    }
    {
        EgtbMaterial job1 = {5, 0, 2, 0};
        EgtbMaterial job2 = {5, 0, 1, 1};
        EgtbMaterial job4 = {4, 1, 2, 0};
        if (egtb_material_generation_compare(&job1, &job2) >= 0 ||
            egtb_material_generation_compare(&job2, &job4) >= 0) {
            fprintf(stderr, "GWD job ordering failed\n");
            return EXIT_FAILURE;
        }
    }
    printf("material catalog tests: PASS (210 canonical, 196 mirrors)\n");
    return EXIT_SUCCESS;
}
