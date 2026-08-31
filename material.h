#ifndef MATERIAL_H
#define MATERIAL_H

#include "movegen.h"

#include <stdbool.h>

#define EGTB_MAX_PIECES 8

typedef struct {
    unsigned white_kings;
    unsigned white_men;
    unsigned black_kings;
    unsigned black_men;
} EgtbMaterial;

typedef enum {
    EGTB_MATERIAL_INVALID,
    EGTB_MATERIAL_TERMINAL,
    EGTB_MATERIAL_CANONICAL,
    EGTB_MATERIAL_MIRROR
} EgtbMaterialKind;

EgtbMaterialKind egtb_material_resolve(const EgtbMaterial *requested,
                                       EgtbMaterial *canonical);
EgtbMaterial egtb_position_material(const DraughtsPosition *position);
void egtb_mirror_position(const DraughtsPosition *source,
                          DraughtsPosition *destination);
EgtbSide egtb_mirror_side(EgtbSide side);

/* GWD job order: pieces ascending; larger side first; kings descending. */
int egtb_material_generation_compare(const EgtbMaterial *a,
                                     const EgtbMaterial *b);

#endif
