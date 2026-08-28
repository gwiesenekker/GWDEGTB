#ifndef COMBINATORIAL_INDEX_H
#define COMBINATORIAL_INDEX_H

#include "endgame_index.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    COMB_REGION_TOP,
    COMB_REGION_MIDDLE,
    COMB_REGION_BOTTOM,
    COMB_REGION_COUNT,
    COMB_PIECE_COUNT = 4
};

typedef struct {
    uint8_t counts[COMB_REGION_COUNT][COMB_PIECE_COUNT];
    uint16_t label_count[COMB_REGION_COUNT];
    uint16_t label_scheme[COMB_REGION_COUNT];
    uint64_t arrangements[COMB_REGION_COUNT];
    uint64_t offset;
    uint64_t size;
} CombDistribution;

typedef struct {
    uint8_t counts[COMB_PIECE_COUNT];
    uint8_t occupied;
    uint16_t label_count;
    uint16_t *rank_by_code;
    uint16_t *code_by_rank;
} CombLabelScheme;

typedef struct {
    unsigned white_men;
    unsigned black_men;
    unsigned white_kings;
    unsigned black_kings;
    uint64_t position_count;
    CombDistribution *distributions;
    size_t distribution_count;
    CombLabelScheme *label_schemes;
    size_t label_scheme_count;
    size_t label_scheme_capacity;
    uint32_t *distribution_by_key;
    size_t key_count;
} CombinatorialIndexer;

bool comb_indexer_init(CombinatorialIndexer *indexer,
                       unsigned white_men, unsigned black_men,
                       unsigned white_kings, unsigned black_kings);
void comb_indexer_destroy(CombinatorialIndexer *indexer);

uint64_t comb_position_count(const CombinatorialIndexer *indexer);
bool comb_position_to_index(const CombinatorialIndexer *indexer,
                            const EgPosition *position, uint64_t *index);
bool comb_index_to_position(const CombinatorialIndexer *indexer,
                            uint64_t index, EgPosition *position);

#endif
