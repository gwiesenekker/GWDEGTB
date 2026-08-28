#ifndef ENDGAME_INDEX_H
#define ENDGAME_INDEX_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t white_men;
    uint64_t black_men;
    uint64_t white_kings;
    uint64_t black_kings;
} EgPosition;

typedef struct {
    unsigned white_men;
    unsigned black_men;
    unsigned white_kings;
    unsigned black_kings;
    uint64_t *ways;
    uint64_t *rank_add;
    unsigned char *overflow;
    uint64_t piece_stride[4];
    uint64_t square_base[50];
    uint64_t square_stride;
    uint64_t position_count;
    uint64_t table_size;
} EgIndexer;

/*
 * Build an indexer for one fixed material configuration.  Returns false if
 * the material is impossible, allocation fails, or the number of positions
 * does not fit in uint64_t.
 */
bool eg_indexer_init(EgIndexer *indexer,
                     unsigned white_men, unsigned black_men,
                     unsigned white_kings, unsigned black_kings);
void eg_indexer_destroy(EgIndexer *indexer);

uint64_t eg_position_count(const EgIndexer *indexer);
uint64_t eg_max_index(const EgIndexer *indexer);

/*
 * Rank a legal position in [0, eg_position_count()).  Argument and position
 * validation is compiled out when NDEBUG is defined.
 */
bool eg_position_to_index(const EgIndexer *indexer,
                          const EgPosition *position, uint64_t *index);

/* Recover the unique position represented by index. Bounds checks are debug-only. */
bool eg_index_to_position(const EgIndexer *indexer,
                          uint64_t index, EgPosition *position);

/* Exhaustively enumerate independently and check both conversions. */
bool eg_test_material(unsigned white_men, unsigned black_men,
                      unsigned white_kings, unsigned black_kings,
                      uint64_t *positions_tested);

#endif
