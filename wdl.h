#ifndef WDL_H
#define WDL_H

#include "egtb.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WDL_FORMAT_VERSION 1
#define WDL_PAGE_SIZE 1024

typedef enum {
    WDL_DRAW = 0,
    WDL_WIN = 1,
    WDL_LOSS = 2
} WdlResult;

typedef struct {
    uint64_t wins[2];
    uint64_t draws[2];
    uint64_t losses[2];
} WdlStatistics;

typedef struct {
    uint64_t logical_uncompressed_bytes;
    uint64_t compressed_payload_bytes;
    uint64_t file_bytes;
    uint64_t stored_pages;
} WdlStorageStatistics;

typedef struct Wdl Wdl;

const char *wdl_last_error(void);

/* Compile a complete DTM EGTB into a packed, compressed WDL file. */
bool wdl_compile(const char *dtm_path, const char *wdl_path,
                 int compression_level, size_t dtm_cache_pages,
                 WdlStatistics *statistics,
                 WdlStorageStatistics *storage_statistics);

/*
 * Open path read-only. If a .wdl path does not exist, derive the corresponding
 * .dtm path, compile it, then open the newly created WDL file.
 */
bool wdl_open(Wdl **out, const char *path, size_t cache_pages,
              int compression_level, size_t dtm_cache_pages);
bool wdl_close(Wdl *wdl);
bool wdl_get(Wdl *wdl, uint64_t index, EgtbSide side, WdlResult *result);

/* Decompress the complete packed WDL bitmap into caller-owned storage. */
bool wdl_decompress_into(Wdl *wdl, void *data, size_t size);

uint64_t wdl_maximum_index(const Wdl *wdl);
uint64_t wdl_page_count(const Wdl *wdl);
uint32_t wdl_page_size(const Wdl *wdl);
bool wdl_storage_statistics(const Wdl *wdl,
                            WdlStorageStatistics *statistics);

#endif
