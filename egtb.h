#ifndef EGTB_H
#define EGTB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EGTB_FORMAT_VERSION 1
#define EGTB_DRAW INT16_C(-1)

typedef enum {
    EGTB_WHITE_TO_MOVE = 0,
    EGTB_BLACK_TO_MOVE = 1
} EgtbSide;

typedef struct {
    int16_t white_to_move;
    int16_t black_to_move;
} EgtbEntry;

typedef struct Egtb Egtb;

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

const char *egtb_last_error(void);

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

uint64_t egtb_maximum_index(const Egtb *egtb);
uint64_t egtb_page_count(const Egtb *egtb);
uint32_t egtb_page_size(const Egtb *egtb);
bool egtb_is_readonly(const Egtb *egtb);
unsigned egtb_reserve_percent(const Egtb *egtb);

/* Flush a writable EGTB before calling this if dirty pages must be included. */
bool egtb_storage_statistics(Egtb *egtb, EgtbStorageStatistics *statistics);

/* Rewrite all live pages into a hole-free temporary file and atomically replace it. */
bool egtb_compact(const char *path, int compression_level,
                  size_t source_cache_pages);

#endif
