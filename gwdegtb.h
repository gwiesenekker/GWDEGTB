#ifndef GWDEGTB_H
#define GWDEGTB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GWDEGTB_WDL_UNAVAILABLE INT16_MIN
#define GWDEGTB_WDL_DRAW INT16_C(-1)
#define GWDEGTB_WDL_LOSS INT16_C(0)
#define GWDEGTB_WDL_WIN INT16_C(1)
#define GWDEGTB_DTM_UNAVAILABLE INT16_MIN

/* The playable fields in GWD's padded 64-bit board representation. */
#define GWDEGTB_GWD_BOARD_MASK UINT64_C(0x0ffdffbff7feffc0)
/* Squares 0..49 in GWDEGTB's compact 64-bit board representation. */
#define GWDEGTB_COMPACT_BOARD_MASK ((UINT64_C(1) << 50) - 1)

typedef enum {
    GWDEGTB_WHITE_TO_MOVE = 0,
    GWDEGTB_BLACK_TO_MOVE = 1
} GwdegtbSide;

typedef struct GwdegtbWdlProbe GwdegtbWdlProbe;

typedef struct {
    uint64_t requested_cache_bytes;
    uint64_t allocated_cache_bytes;
    uint64_t cache_entries;
    uint64_t lookups;
    uint64_t hits;
    uint64_t misses;
    uint64_t decompressions;
} GwdegtbWdlProbeStatistics;

/* Convert GWD fields 6..59, including row guards, to squares 0..49. */
uint64_t gwdegtb_gwd_to_compact(uint64_t gwd_bitboard);

/* Calculate the dense maximum index and packed resident allocation size.
 * Supports 2..8 pieces, with at least one piece of each colour. */
bool gwdegtb_wdl_info(const char *database_name, uint64_t *maximum_index,
                      size_t *size);

/*
 * Decompress into exactly size bytes of caller-owned memory. If the canonical
 * .wdl file does not exist, it is generated atomically from the corresponding
 * .dtm file before decompression. This does not register the bitmap; call
 * gwdegtb_wdl_attach() after synchronization. directory may be NULL or empty.
 * A mirrored basename resolves to the corresponding canonical WDL file.
 * Decompression uses four workers by default.
 */
bool gwdegtb_wdl_decompress(const char *directory,
                            const char *database_name,
                            void *data, size_t size);

/* As above, with an explicit decompression worker count. */
bool gwdegtb_wdl_decompress_threads(const char *directory,
                                    const char *database_name,
                                    void *data, size_t size,
                                    unsigned thread_count);

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

/* As above, accepting compact bitboards with playable squares in bits 0..49. */
int16_t gwdegtb_wdl_lookup_compact(uint64_t white_kings,
                                   uint64_t white_men,
                                   uint64_t black_kings,
                                   uint64_t black_men,
                                   GwdegtbSide side);

/*
 * Compressed-resident WDL tier. The complete .wdl file image is stored in
 * caller-owned memory, which may be an MPI shared window. compressed_info()
 * ensures that the canonical WDL exists (generating it from DTM if needed)
 * and returns its exact file-image size. If neither file exists, the call
 * succeeds with size zero so configured but not-yet-generated databases can
 * be skipped. Invalid input and actual filesystem/generation errors fail.
 * One process loads the image; every process attaches its local pointer after
 * synchronization. Attached storage is never freed by GWDEGTB.
 */
bool gwdegtb_wdl_compressed_info(const char *directory,
                                 const char *database_name,
                                 size_t *size);
bool gwdegtb_wdl_compressed_load(const char *directory,
                                 const char *database_name,
                                 void *data, size_t size);
bool gwdegtb_wdl_compressed_attach(const char *database_name,
                                   const void *data, size_t size);
bool gwdegtb_wdl_compressed_is_loaded(unsigned white_kings,
                                      unsigned white_men,
                                      unsigned black_kings,
                                      unsigned black_men);

/*
 * Create one mutable cache for one calling thread. cache_bytes is a total
 * budget shared by every compressed WDL attached to the process. A probe must
 * never be used concurrently by two threads. Destroy all probes before
 * unloading compressed WDL handles or releasing their shared storage.
 */
bool gwdegtb_wdl_probe_create(size_t cache_bytes,
                              GwdegtbWdlProbe **probe);
void gwdegtb_wdl_probe_destroy(GwdegtbWdlProbe *probe);
int16_t gwdegtb_wdl_lookup_probe(GwdegtbWdlProbe *probe,
                                 uint64_t white_kings,
                                 uint64_t white_men,
                                 uint64_t black_kings,
                                 uint64_t black_men,
                                 GwdegtbSide side);
/* Probe an attached compressed WDL using compact square-0..49 bitboards. */
int16_t gwdegtb_wdl_lookup_probe_compact(GwdegtbWdlProbe *probe,
                                         uint64_t white_kings,
                                         uint64_t white_men,
                                         uint64_t black_kings,
                                         uint64_t black_men,
                                         GwdegtbSide side);
void gwdegtb_wdl_probe_statistics(
    const GwdegtbWdlProbe *probe,
    GwdegtbWdlProbeStatistics *statistics);
void gwdegtb_wdl_compressed_unload_all(void);

/* Call after the root search and PV construction have stopped probing DTM. */
void gwdegtb_dtm_close_all(void);

/*
 * Look up exact DTM from GWD padded bitboards. Material, file selection and
 * mirroring are automatic. The canonical file is opened and assigned an
 * uncompressed cache of cache_bytes on its first lookup. Returns odd positive
 * plies for a win, zero or an even negative value for a loss, -1 for a draw,
 * or INT16_MIN when unavailable or invalid. A side to move with no pieces is
 * terminal lost-in-0. directory may be NULL or empty for the current directory.
 * This API is deliberately single-threaded: each open database owns one cache.
 */
int16_t gwdegtb_dtm_lookup(const char *directory, size_t cache_bytes,
                           uint64_t white_kings, uint64_t white_men,
                           uint64_t black_kings, uint64_t black_men,
                           GwdegtbSide side);

/* As above, accepting compact bitboards with playable squares in bits 0..49. */
int16_t gwdegtb_dtm_lookup_compact(
    const char *directory, size_t cache_bytes,
    uint64_t white_kings, uint64_t white_men,
    uint64_t black_kings, uint64_t black_men,
    GwdegtbSide side);

const char *gwdegtb_last_error(void);

#endif
