#ifndef EGTB_H
#define EGTB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EGTB_FORMAT_VERSION 2
#define EGTB_DRAW INT16_C(-1)
#define EGTB_MAX_WIN_DTM INT16_C(253)
#define EGTB_MAX_LOSS_DTM INT16_C(254)
#define EGTB_STORED_DRAW INT8_MIN

typedef enum {
    EGTB_WHITE_TO_MOVE = 0,
    EGTB_BLACK_TO_MOVE = 1
} EgtbSide;

typedef struct {
    int8_t white_to_move;
    int8_t black_to_move;
} EgtbEntry;

typedef struct Egtb Egtb;
typedef struct EgtbView EgtbView;
typedef struct EgtbResident EgtbResident;

/*
 * Forward-only paired-entry reader. The cache view must not be used by any
 * other lookup while a reader is active, because the reader retains a direct
 * pointer into the view's current cache page.
 */
typedef struct {
    EgtbView *view;
    uint64_t next_index;
    uint64_t end_index;
    const EgtbEntry *next_entry;
    const EgtbEntry *page_end;
} EgtbSequentialReader;

typedef struct {
    size_t cache_pages;
    unsigned reserve_percent;
    int compression_level;
} EgtbCreateOptions;

typedef struct {
    uint64_t logical_uncompressed_bytes;
    uint64_t compressed_payload_bytes;
    uint64_t live_block_bytes;
    uint64_t file_bytes;
    uint64_t live_pages;
} EgtbStorageStatistics;

typedef struct {
    uint64_t lookups;
    uint64_t hits;
    uint64_t misses;
    uint64_t decompressions;
    uint64_t dirty_evictions;
    uint64_t compressed_writes;
} EgtbCacheStatistics;

const char *egtb_last_error(void);

/* Convert between exact public ply values and the version-2 signed byte. */
bool egtb_encode_dtm(int16_t value, int8_t *stored);
int16_t egtb_decode_dtm(int8_t stored);

/* Build "<wk>wX-<wm>wO-<bk>bX-<bm>bO.<extension>" (GWD order). */
bool egtb_material_filename(char *buffer, size_t buffer_size,
                            unsigned white_kings, unsigned white_men,
                            unsigned black_kings, unsigned black_men,
                            const char *extension);

/* Create a new, all-draw, read/write EGTB. Existing files are not overwritten. */
bool egtb_create(Egtb **out, const char *path, uint64_t maximum_index,
                 uint32_t page_size, const EgtbCreateOptions *options);

/* Open an existing EGTB. Read-only handles are shared by path and ref-counted. */
bool egtb_open_readonly(Egtb **out, const char *path, size_t cache_pages);
bool egtb_open_readwrite(Egtb **out, const char *path, size_t cache_pages);

/* Flush and release a handle. For shared read-only handles this drops one reference. */
bool egtb_close(Egtb *egtb);
bool egtb_flush(Egtb *egtb);

bool egtb_get(Egtb *egtb, uint64_t index, EgtbSide side, int16_t *value);
bool egtb_set(Egtb *egtb, uint64_t index, EgtbSide side, int16_t value);

/*
 * Create a direct-mapped cache view over an open EGTB backing. Read-only
 * views may coexist. A writable view requires a writable backing and is
 * exclusive until page ownership is introduced by the threaded generator.
 */
bool egtb_view_create(EgtbView **out, Egtb *backing, size_t cache_pages,
                      bool writable);
bool egtb_view_create_range(EgtbView **out, Egtb *backing,
                            size_t cache_pages, bool writable,
                            uint64_t first_page, uint64_t end_page);
bool egtb_view_close(EgtbView *view);
bool egtb_view_flush(EgtbView *view);
bool egtb_view_get(EgtbView *view, uint64_t index, EgtbSide side,
                   int16_t *value);
bool egtb_view_get_pair(EgtbView *view, uint64_t index,
                        int16_t *white_to_move, int16_t *black_to_move);
bool egtb_view_set(EgtbView *view, uint64_t index, EgtbSide side,
                   int16_t value);
bool egtb_sequential_reader_init(EgtbSequentialReader *reader,
                                 EgtbView *view, uint64_t first_index,
                                 uint64_t end_index);
bool egtb_sequential_reader_next(EgtbSequentialReader *reader,
                                 int16_t *white_to_move,
                                 int16_t *black_to_move);
void egtb_view_cache_statistics(const EgtbView *view,
                                EgtbCacheStatistics *statistics);

/* Parallel, checksum-verifying decompression into a flat read-only array. */
bool egtb_resident_load(EgtbResident **out, Egtb *backing,
                        unsigned thread_count);
void egtb_resident_destroy(EgtbResident *resident);
bool egtb_resident_get(const EgtbResident *resident, uint64_t index,
                       EgtbSide side, int16_t *value);
bool egtb_resident_get_pair(const EgtbResident *resident, uint64_t index,
                            int16_t *white_to_move,
                            int16_t *black_to_move);
bool egtb_resident_matches(const EgtbResident *resident,
                           const Egtb *backing);
uint64_t egtb_resident_bytes(const EgtbResident *resident);
/* Produces decoded int16_t DTM bins, indexed by (uint16_t)dtm. */
bool egtb_resident_dtm_histogram(const EgtbResident *resident,
                                 uint64_t *histogram,
                                 size_t bins_per_side);

uint64_t egtb_maximum_index(const Egtb *egtb);
uint64_t egtb_page_count(const Egtb *egtb);
uint32_t egtb_page_size(const Egtb *egtb);
bool egtb_is_readonly(const Egtb *egtb);
unsigned egtb_reserve_percent(const Egtb *egtb);
size_t egtb_cache_pages(const Egtb *egtb);
bool egtb_resize_cache(Egtb *egtb, size_t cache_pages);

/* Flush a writable EGTB before calling this if dirty pages must be included. */
bool egtb_storage_statistics(Egtb *egtb, EgtbStorageStatistics *statistics);

/* Rewrite all live pages into a hole-free temporary file and atomically replace it. */
bool egtb_compact(const char *path, int compression_level,
                  size_t source_cache_pages);

#endif
