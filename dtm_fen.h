#ifndef DTM_FEN_H
#define DTM_FEN_H

#include "egtb.h"
#include "endgame_index.h"

#include <stdbool.h>
#include <stddef.h>

/* Format one indexed position as PDN FEN followed by its exact DTM in braces. */
bool egtb_format_dtm_fen(const EgIndexer *indexer, uint64_t index,
                         EgtbSide side, int16_t dtm,
                         char *buffer, size_t size);

#endif
