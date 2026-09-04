#include "dtm_fen.h"

#include <stdio.h>

static bool append_pieces(char *buffer, size_t size, size_t *used,
                          uint64_t men, uint64_t kings)
{
    bool first = true;
    for (unsigned kind = 0; kind < 2; ++kind) {
        uint64_t pieces = kind == 0 ? men : kings;
        while (pieces != 0) {
            unsigned square = (unsigned)__builtin_ctzll(pieces);
            int written = snprintf(buffer + *used, size - *used,
                                   "%s%s%u", first ? "" : ",",
                                   kind == 0 ? "" : "K", square + 1);
            if (written < 0 || (size_t)written >= size - *used)
                return false;
            *used += (size_t)written;
            first = false;
            pieces &= pieces - 1;
        }
    }
    return true;
}

bool egtb_format_dtm_fen(const EgIndexer *indexer, uint64_t index,
                         EgtbSide side, int16_t dtm,
                         char *buffer, size_t size)
{
    EgPosition position;
    size_t used;
    int written;
    if (indexer == NULL || buffer == NULL || size == 0 ||
        (side != EGTB_WHITE_TO_MOVE && side != EGTB_BLACK_TO_MOVE) ||
        !eg_index_to_position(indexer, index, &position))
        return false;
    written = snprintf(buffer, size, "%c:W",
                       side == EGTB_WHITE_TO_MOVE ? 'W' : 'B');
    if (written < 0 || (size_t)written >= size)
        return false;
    used = (size_t)written;
    if (!append_pieces(buffer, size, &used, position.white_men,
                       position.white_kings))
        return false;
    written = snprintf(buffer + used, size - used, ":B");
    if (written < 0 || (size_t)written >= size - used)
        return false;
    used += (size_t)written;
    if (!append_pieces(buffer, size, &used, position.black_men,
                       position.black_kings))
        return false;
    written = snprintf(buffer + used, size - used, " {%d}", dtm);
    return written >= 0 && (size_t)written < size - used;
}
