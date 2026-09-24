#ifndef DTM_PV_H
#define DTM_PV_H
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* Single-threaded diagnostic API. Reads existing DTM files only, bypassing
 * page/result caches (OS buffering still applies). Writes a move sequence,
 * wrapped at 80 columns, with {pre-move DTM, other optimal moves} PDN comments.
 * Alternatives are separated by "or" and omitted when the best move is unique.
 * Long comments wrap between alternatives.
 * Captures use endpoint notation. Ends with 2-0/0-2, or 1-1 for an immediate draw.
 * On failure output may contain a partial PV; error explains why.
 * FEN uses W/B, K prefixes, comma-separated squares and optional final period.
 * Do not concurrently probe the same files through other library APIs.
 */
bool gwdegtb_dtm_pv(const char *directory, const char *fen, FILE *output,
                    char *error, size_t error_size);
#endif
