#ifndef GWDEGTB_H
#define GWDEGTB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GWDEGTB_WDL_UNAVAILABLE INT16_MIN
#define GWDEGTB_WDL_DRAW INT16_C(-1)
#define GWDEGTB_WDL_LOSS INT16_C(0)
#define GWDEGTB_WDL_WIN INT16_C(1)

/* The playable fields in GWD's padded 64-bit board representation. */
#define GWDEGTB_GWD_BOARD_MASK UINT64_C(0x0ffdffbff7feffc0)

typedef enum {
    GWDEGTB_WHITE_TO_MOVE = 0,
    GWDEGTB_BLACK_TO_MOVE = 1
} GwdegtbSide;

/* Convert GWD fields 6..59, including row guards, to squares 0..49. */
uint64_t gwdegtb_gwd_to_compact(uint64_t gwd_bitboard);

/* Calculate the dense maximum index and packed resident allocation size. */
bool gwdegtb_wdl_info(const char *database_name, uint64_t *maximum_index,
                      size_t *size);

/*
 * Decompress into exactly size bytes of caller-owned memory. If the canonical
 * .wdl file does not exist, it is generated atomically from the corresponding
 * .dtm file before decompression. This does not register the bitmap; call
 * gwdegtb_wdl_attach() after synchronization. directory may be NULL or empty.
 * A mirrored basename resolves to the corresponding canonical WDL file.
 */
bool gwdegtb_wdl_decompress(const char *directory,
                            const char *database_name,
                            void *data, size_t size);

/*
 * Replace/register a resident bitmap with storage owned by GWD (for example
 * an MPI shared window). Call this on every rank after the master has
 * decompressed the bitmap. The byte count is checked against the material's
 * dense index.
 * GWDEGTB never frees attached storage.
 */
bool gwdegtb_wdl_attach(const char *database_name,
                        const void *data, size_t size);

bool gwdegtb_wdl_is_loaded(unsigned white_kings, unsigned white_men,
                           unsigned black_kings, unsigned black_men);

/*
 * Call only after all search threads have stopped using resident WDL data,
 * and before GWD releases any attached MPI shared windows.
 */
void gwdegtb_wdl_unload_all(void);

/*
 * Look up GWD padded bitboards. Material and mirroring are derived
 * automatically. Returns 1 (win), 0 (loss), -1 (draw), or -32768 when the
 * required WDL is unavailable or the position/side is invalid.
 */
int16_t gwdegtb_wdl_lookup(uint64_t white_kings, uint64_t white_men,
                           uint64_t black_kings, uint64_t black_men,
                           GwdegtbSide side);

const char *gwdegtb_last_error(void);

#endif
