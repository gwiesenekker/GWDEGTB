#define _POSIX_C_SOURCE 200809L

#include "wdl.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <zstd.h>

#define WDL_HEADER_SIZE 64
#define WDL_DIRECTORY_ENTRY_SIZE 14
#define WDL_POSITIONS_PER_PAGE (WDL_PAGE_SIZE * 2)
#define WDL_INVALID_PAGE UINT64_MAX

typedef struct {
    uint64_t page_number;
    unsigned char data[WDL_PAGE_SIZE];
} WdlCacheEntry;

struct Wdl {
    FILE *file;
    uint64_t maximum_index;
    uint64_t page_count;
    uint64_t packed_bytes;
    uint64_t data_offset;
    uint64_t file_bytes;
    uint64_t *offsets;
    uint16_t *lengths;
    uint32_t *checksums;
    WdlCacheEntry *cache;
    size_t cache_pages;
    ZSTD_DCtx *decompressor;
    unsigned char *compressed;
    size_t compressed_capacity;
};

static const unsigned char wdl_magic[8] = {'I','P','D','W','D','L','\0','\0'};
static char last_error[256];

static bool fail(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(last_error, sizeof(last_error), format, arguments);
    va_end(arguments);
    return false;
}

const char *wdl_last_error(void)
{
    return last_error;
}

static void put_u16(unsigned char *p, uint16_t value)
{
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
}

static void put_u32(unsigned char *p, uint32_t value)
{
    unsigned i;
    for (i = 0; i < 4; ++i)
        p[i] = (unsigned char)(value >> (8 * i));
}

static void put_u64(unsigned char *p, uint64_t value)
{
    unsigned i;
    for (i = 0; i < 8; ++i)
        p[i] = (unsigned char)(value >> (8 * i));
}

static uint16_t get_u16(const unsigned char *p)
{
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}

static uint32_t get_u32(const unsigned char *p)
{
    uint32_t value = 0;
    unsigned i;
    for (i = 0; i < 4; ++i)
        value |= (uint32_t)p[i] << (8 * i);
    return value;
}

static uint64_t get_u64(const unsigned char *p)
{
    uint64_t value = 0;
    unsigned i;
    for (i = 0; i < 8; ++i)
        value |= (uint64_t)p[i] << (8 * i);
    return value;
}

static uint32_t crc32c(const void *data, size_t size)
{
    static uint32_t table[256];
    static bool initialized;
    const unsigned char *bytes = data;
    uint32_t crc = UINT32_MAX;
    size_t i;

    if (!initialized) {
        unsigned entry;
        for (entry = 0; entry < 256; ++entry) {
            uint32_t value = entry;
            unsigned bit;
            for (bit = 0; bit < 8; ++bit)
                value = (value >> 1) ^
                        (UINT32_C(0x82f63b78) &
                         (UINT32_C(0) - (value & 1)));
            table[entry] = value;
        }
        initialized = true;
    }
    for (i = 0; i < size; ++i)
        crc = table[(crc ^ bytes[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}

static bool seek_file(FILE *file, uint64_t offset)
{
    if (offset > (uint64_t)INT64_MAX)
        return fail("WDL file offset is too large");
    if (fseeko(file, (off_t)offset, SEEK_SET) != 0)
        return fail("WDL file seek failed: %s", strerror(errno));
    return true;
}

static bool read_at(FILE *file, uint64_t offset, void *data, size_t size)
{
    return seek_file(file, offset) &&
           (fread(data, 1, size, file) == size ||
            fail("WDL file read failed or was truncated"));
}

static bool write_all(FILE *file, const void *data, size_t size)
{
    return fwrite(data, 1, size, file) == size ||
           fail("WDL file write failed");
}

static WdlResult dtm_to_wdl(int16_t dtm)
{
    if (dtm == EGTB_DRAW)
        return WDL_DRAW;
    return dtm > 0 ? WDL_WIN : WDL_LOSS;
}

static void count_result(WdlStatistics *statistics, EgtbSide side,
                         WdlResult result)
{
    if (result == WDL_WIN)
        ++statistics->wins[side];
    else if (result == WDL_LOSS)
        ++statistics->losses[side];
    else
        ++statistics->draws[side];
}

static bool replace_extension(const char *path, const char *old_extension,
                              const char *new_extension, char **result)
{
    size_t path_length = strlen(path);
    size_t old_length = strlen(old_extension);
    size_t new_length = strlen(new_extension);
    char *replacement;
    if (path_length < old_length ||
        strcmp(path + path_length - old_length, old_extension) != 0)
        return fail("WDL path must end in %s", old_extension);
    if (path_length - old_length > SIZE_MAX - new_length - 1)
        return fail("WDL path is too long");
    replacement = malloc(path_length - old_length + new_length + 1);
    if (replacement == NULL)
        return fail("cannot allocate derived EGTB path");
    memcpy(replacement, path, path_length - old_length);
    memcpy(replacement + path_length - old_length, new_extension,
           new_length + 1);
    *result = replacement;
    return true;
}

static bool write_wdl_file(const char *path, const uint64_t *bitmap,
                           uint64_t maximum_index, int compression_level,
                           WdlStorageStatistics *statistics)
{
    uint64_t positions = maximum_index + 1;
    uint64_t packed_bytes = positions / 2 + (positions % 2 != 0);
    uint64_t page_count = packed_bytes / WDL_PAGE_SIZE +
                          (packed_bytes % WDL_PAGE_SIZE != 0);
    uint64_t directory_bytes, data_offset, page;
    uint64_t *offsets = NULL;
    uint16_t *lengths = NULL;
    uint32_t *checksums = NULL;
    unsigned char *compressed = NULL;
    unsigned char page_data[WDL_PAGE_SIZE];
    unsigned char header[WDL_HEADER_SIZE] = {0};
    unsigned char directory_entry[WDL_DIRECTORY_ENTRY_SIZE];
    size_t compressed_capacity = ZSTD_compressBound(WDL_PAGE_SIZE);
    ZSTD_CCtx *compressor = NULL;
    FILE *file = NULL;
    char *temporary = NULL;
    int descriptor = -1;
    bool ok = false;

    if (page_count > SIZE_MAX / sizeof(*offsets) ||
        page_count > (UINT64_MAX - WDL_HEADER_SIZE) /
                         WDL_DIRECTORY_ENTRY_SIZE)
        return fail("WDL directory is too large");
    directory_bytes = page_count * WDL_DIRECTORY_ENTRY_SIZE;
    data_offset = WDL_HEADER_SIZE + directory_bytes;
    if (data_offset > (uint64_t)INT64_MAX)
        return fail("WDL file is too large for this platform");
    offsets = calloc((size_t)page_count, sizeof(*offsets));
    lengths = calloc((size_t)page_count, sizeof(*lengths));
    checksums = calloc((size_t)page_count, sizeof(*checksums));
    compressed = malloc(compressed_capacity);
    compressor = ZSTD_createCCtx();
    temporary = malloc(strlen(path) + 24);
    if (offsets == NULL || lengths == NULL || checksums == NULL ||
        compressed == NULL || compressor == NULL || temporary == NULL) {
        fail("cannot allocate WDL compilation data");
        goto done;
    }
    snprintf(temporary, strlen(path) + 24, "%s.tmp.XXXXXX", path);
    descriptor = mkstemp(temporary);
    if (descriptor < 0) {
        fail("cannot create temporary WDL file: %s", strerror(errno));
        goto done;
    }
    file = fdopen(descriptor, "w+b");
    if (file == NULL) {
        close(descriptor);
        descriptor = -1;
        fail("cannot create WDL stream: %s", strerror(errno));
        goto done;
    }
    descriptor = -1;

    memcpy(header, wdl_magic, sizeof(wdl_magic));
    header[8] = WDL_FORMAT_VERSION;
    put_u16(header + 10, WDL_HEADER_SIZE);
    put_u32(header + 12, WDL_PAGE_SIZE);
    put_u64(header + 16, maximum_index);
    put_u64(header + 24, page_count);
    put_u64(header + 32, WDL_HEADER_SIZE);
    put_u64(header + 40, data_offset);
    put_u64(header + 48, packed_bytes);
    if (!write_all(file, header, sizeof(header)) ||
        ftruncate(fileno(file), (off_t)data_offset) != 0) {
        fail("cannot initialize WDL file: %s", strerror(errno));
        goto done;
    }

    for (page = 0; page < page_count; ++page) {
        uint64_t first_byte = page * WDL_PAGE_SIZE;
        size_t bytes = (size_t)(packed_bytes - first_byte);
        size_t word, words;
        bool all_draw = true;
        if (bytes > WDL_PAGE_SIZE)
            bytes = WDL_PAGE_SIZE;
        memset(page_data, 0, sizeof(page_data));
        words = (bytes + 7) / 8;
        for (word = 0; word < words; ++word) {
            uint64_t value = bitmap[first_byte / 8 + word];
            size_t byte;
            for (byte = 0; byte < 8 && word * 8 + byte < bytes; ++byte) {
                unsigned char value_byte =
                    (unsigned char)(value >> (8 * byte));
                page_data[word * 8 + byte] = value_byte;
                if (value_byte != 0)
                    all_draw = false;
            }
        }
        if (!all_draw) {
            size_t compressed_size = ZSTD_compressCCtx(
                compressor, compressed, compressed_capacity, page_data,
                sizeof(page_data), compression_level);
            off_t offset;
            if (ZSTD_isError(compressed_size) || compressed_size == 0 ||
                compressed_size > UINT16_MAX) {
                fail("WDL Zstd compression failed: %s",
                     ZSTD_getErrorName(compressed_size));
                goto done;
            }
            if (fseeko(file, 0, SEEK_END) != 0 ||
                (offset = ftello(file)) < 0) {
                fail("cannot seek in WDL output");
                goto done;
            }
            offsets[page] = (uint64_t)offset;
            lengths[page] = (uint16_t)compressed_size;
            checksums[page] = crc32c(page_data, sizeof(page_data));
            if (!write_all(file, compressed, compressed_size))
                goto done;
        }
    }

    for (page = 0; page < page_count; ++page) {
        put_u64(directory_entry, offsets[page]);
        put_u16(directory_entry + 8, lengths[page]);
        put_u32(directory_entry + 10, checksums[page]);
        if (!seek_file(file, WDL_HEADER_SIZE +
                             page * WDL_DIRECTORY_ENTRY_SIZE) ||
            !write_all(file, directory_entry, sizeof(directory_entry)))
            goto done;
    }
    if (fflush(file) != 0 || fsync(fileno(file)) != 0) {
        fail("cannot flush WDL output: %s", strerror(errno));
        goto done;
    }
    if (fclose(file) != 0) {
        file = NULL;
        fail("cannot close WDL output: %s", strerror(errno));
        goto done;
    }
    file = NULL;
    if (rename(temporary, path) != 0) {
        fail("cannot install WDL file: %s", strerror(errno));
        goto done;
    }

    if (statistics != NULL) {
        struct stat status;
        memset(statistics, 0, sizeof(*statistics));
        statistics->logical_uncompressed_bytes = packed_bytes;
        for (page = 0; page < page_count; ++page) {
            if (offsets[page] != 0) {
                ++statistics->stored_pages;
                statistics->compressed_payload_bytes += lengths[page];
            }
        }
        if (stat(path, &status) != 0) {
            fail("cannot stat WDL output: %s", strerror(errno));
            goto done;
        }
        statistics->file_bytes = (uint64_t)status.st_size;
    }
    ok = true;

done:
    if (file != NULL)
        fclose(file);
    if (descriptor >= 0)
        close(descriptor);
    if (!ok && temporary != NULL)
        unlink(temporary);
    free(temporary);
    ZSTD_freeCCtx(compressor);
    free(compressed);
    free(checksums);
    free(lengths);
    free(offsets);
    return ok;
}

bool wdl_compile(const char *dtm_path, const char *wdl_path,
                 int compression_level, size_t dtm_cache_pages,
                 WdlStatistics *statistics,
                 WdlStorageStatistics *storage_statistics)
{
    Egtb *dtm = NULL;
    uint64_t positions, word_count, index;
    uint64_t *bitmap = NULL;
    WdlStatistics local_statistics;
    bool ok = false;

    if (dtm_path == NULL || wdl_path == NULL || dtm_cache_pages == 0)
        return fail("invalid WDL compilation argument");
    memset(&local_statistics, 0, sizeof(local_statistics));
    if (!egtb_open_readonly(&dtm, dtm_path, dtm_cache_pages))
        return fail("cannot open DTM source: %s", egtb_last_error());
    positions = egtb_maximum_index(dtm) + 1;
    word_count = positions / 16 + (positions % 16 != 0);
    if (word_count > SIZE_MAX / sizeof(*bitmap)) {
        fail("packed WDL bitmap is too large for this process");
        goto done;
    }
    bitmap = calloc((size_t)word_count, sizeof(*bitmap));
    if (bitmap == NULL) {
        fail("cannot allocate packed WDL bitmap");
        goto done;
    }
    for (index = 0; index < positions; ++index) {
        unsigned side;
        for (side = 0; side < 2; ++side) {
            int16_t dtm_value;
            WdlResult result;
            unsigned shift = (unsigned)(index % 16) * 4 + side * 2;
            if (!egtb_get(dtm, index, (EgtbSide)side, &dtm_value)) {
                fail("cannot read DTM source: %s", egtb_last_error());
                goto done;
            }
            result = dtm_to_wdl(dtm_value);
            bitmap[index / 16] |= (uint64_t)result << shift;
            count_result(&local_statistics, (EgtbSide)side, result);
        }
    }
    if (!write_wdl_file(wdl_path, bitmap, positions - 1, compression_level,
                        storage_statistics))
        goto done;
    if (statistics != NULL)
        *statistics = local_statistics;
    ok = true;

done:
    free(bitmap);
    if (!egtb_close(dtm) && ok)
        return fail("cannot close DTM source: %s", egtb_last_error());
    return ok;
}

static void destroy_wdl(Wdl *wdl)
{
    if (wdl == NULL)
        return;
    if (wdl->file != NULL)
        fclose(wdl->file);
    ZSTD_freeDCtx(wdl->decompressor);
    free(wdl->compressed);
    free(wdl->cache);
    free(wdl->checksums);
    free(wdl->lengths);
    free(wdl->offsets);
    free(wdl);
}

static bool open_existing(Wdl **out, const char *path, size_t cache_pages,
                          bool *not_found)
{
    unsigned char header[WDL_HEADER_SIZE];
    unsigned char directory[WDL_DIRECTORY_ENTRY_SIZE * 4096];
    struct stat status;
    Wdl *wdl = NULL;
    uint64_t calculated_pages, page, directory_offset;
    size_t i;

    *out = NULL;
    *not_found = false;
    if (cache_pages == 0)
        return fail("WDL cache must contain at least one page");
    wdl = calloc(1, sizeof(*wdl));
    if (wdl == NULL)
        return fail("cannot allocate WDL handle");
    wdl->file = fopen(path, "rb");
    if (wdl->file == NULL) {
        *not_found = errno == ENOENT;
        fail("cannot open %s: %s", path, strerror(errno));
        goto failure;
    }
    if (!read_at(wdl->file, 0, header, sizeof(header)))
        goto failure;
    if (memcmp(header, wdl_magic, sizeof(wdl_magic)) != 0) {
        fail("not an International Polish Draughts WDL database");
        goto failure;
    }
    if (header[8] != WDL_FORMAT_VERSION) {
        fail("unsupported WDL version %u", (unsigned)header[8]);
        goto failure;
    }
    if (get_u16(header + 10) != WDL_HEADER_SIZE ||
        get_u32(header + 12) != WDL_PAGE_SIZE) {
        fail("unsupported WDL layout");
        goto failure;
    }
    wdl->maximum_index = get_u64(header + 16);
    wdl->page_count = get_u64(header + 24);
    directory_offset = get_u64(header + 32);
    wdl->data_offset = get_u64(header + 40);
    wdl->packed_bytes = get_u64(header + 48);
    if (wdl->maximum_index == UINT64_MAX) {
        fail("invalid WDL maximum index");
        goto failure;
    }
    calculated_pages = wdl->packed_bytes / WDL_PAGE_SIZE +
                       (wdl->packed_bytes % WDL_PAGE_SIZE != 0);
    if (wdl->packed_bytes != (wdl->maximum_index + 1) / 2 +
                                 ((wdl->maximum_index + 1) % 2 != 0) ||
        calculated_pages != wdl->page_count ||
        directory_offset != WDL_HEADER_SIZE ||
        wdl->page_count > (UINT64_MAX - WDL_HEADER_SIZE) /
                              WDL_DIRECTORY_ENTRY_SIZE ||
        wdl->data_offset != WDL_HEADER_SIZE +
                                wdl->page_count * WDL_DIRECTORY_ENTRY_SIZE ||
        wdl->page_count > SIZE_MAX / sizeof(*wdl->offsets)) {
        fail("inconsistent WDL header");
        goto failure;
    }
    if (fstat(fileno(wdl->file), &status) != 0) {
        fail("cannot stat WDL file: %s", strerror(errno));
        goto failure;
    }
    wdl->file_bytes = (uint64_t)status.st_size;
    if (wdl->data_offset > wdl->file_bytes) {
        fail("truncated WDL directory");
        goto failure;
    }
    wdl->offsets = malloc((size_t)wdl->page_count * sizeof(*wdl->offsets));
    wdl->lengths = malloc((size_t)wdl->page_count * sizeof(*wdl->lengths));
    wdl->checksums = malloc((size_t)wdl->page_count * sizeof(*wdl->checksums));
    if (wdl->offsets == NULL || wdl->lengths == NULL ||
        wdl->checksums == NULL) {
        fail("cannot allocate WDL directory");
        goto failure;
    }
    page = 0;
    while (page < wdl->page_count) {
        size_t count = (size_t)(wdl->page_count - page);
        size_t entry;
        if (count > 4096)
            count = 4096;
        if (!read_at(wdl->file, WDL_HEADER_SIZE +
                                    page * WDL_DIRECTORY_ENTRY_SIZE,
                     directory, count * WDL_DIRECTORY_ENTRY_SIZE))
            goto failure;
        for (entry = 0; entry < count; ++entry) {
            const unsigned char *bytes =
                directory + entry * WDL_DIRECTORY_ENTRY_SIZE;
            uint64_t current = page + entry;
            wdl->offsets[current] = get_u64(bytes);
            wdl->lengths[current] = get_u16(bytes + 8);
            wdl->checksums[current] = get_u32(bytes + 10);
            if (wdl->offsets[current] == 0) {
                if (wdl->lengths[current] != 0 ||
                    wdl->checksums[current] != 0) {
                    fail("invalid implicit WDL page %" PRIu64, current);
                    goto failure;
                }
            } else if ((uint64_t)wdl->lengths[current] > wdl->file_bytes ||
                       wdl->offsets[current] < wdl->data_offset ||
                       wdl->lengths[current] == 0 ||
                       wdl->offsets[current] >
                           wdl->file_bytes - wdl->lengths[current]) {
                fail("invalid WDL directory entry for page %" PRIu64,
                     current);
                goto failure;
            }
        }
        page += count;
    }
    if (wdl->page_count < cache_pages)
        cache_pages = (size_t)wdl->page_count;
    if (cache_pages == 0)
        cache_pages = 1;
    if (cache_pages > SIZE_MAX / sizeof(*wdl->cache)) {
        fail("WDL cache is too large");
        goto failure;
    }
    wdl->cache = malloc(cache_pages * sizeof(*wdl->cache));
    wdl->compressed_capacity = ZSTD_compressBound(WDL_PAGE_SIZE);
    wdl->compressed = malloc(wdl->compressed_capacity);
    wdl->decompressor = ZSTD_createDCtx();
    if (wdl->cache == NULL || wdl->compressed == NULL ||
        wdl->decompressor == NULL) {
        fail("cannot allocate WDL cache or decompressor");
        goto failure;
    }
    wdl->cache_pages = cache_pages;
    for (i = 0; i < cache_pages; ++i)
        wdl->cache[i].page_number = WDL_INVALID_PAGE;
    if (crc32c("123456789", 9) != UINT32_C(0xe3069283)) {
        fail("internal WDL CRC32C self-test failed");
        goto failure;
    }
    *out = wdl;
    return true;

failure:
    destroy_wdl(wdl);
    return false;
}

bool wdl_open(Wdl **out, const char *path, size_t cache_pages,
              int compression_level, size_t dtm_cache_pages)
{
    bool not_found;
    char *dtm_path = NULL;
    WdlStatistics statistics;
    WdlStorageStatistics storage;
    bool ok;
    if (out == NULL || path == NULL || cache_pages == 0 ||
        dtm_cache_pages == 0)
        return fail("invalid WDL open argument");
    if (open_existing(out, path, cache_pages, &not_found))
        return true;
    if (!not_found)
        return false;
    if (!replace_extension(path, ".wdl", ".dtm", &dtm_path))
        return false;
    ok = wdl_compile(dtm_path, path, compression_level, dtm_cache_pages,
                     &statistics, &storage);
    free(dtm_path);
    if (!ok)
        return false;
    printf("WDL generated: %s raw=%" PRIu64 " bytes file=%" PRIu64
           " bytes overall=%.2f%% (%.2f:1)\n",
           path, storage.logical_uncompressed_bytes, storage.file_bytes,
           100.0 * (double)storage.file_bytes /
               (double)storage.logical_uncompressed_bytes,
           (double)storage.logical_uncompressed_bytes /
               (double)storage.file_bytes);
    printf("WDL WTM: wins=%" PRIu64 " draws=%" PRIu64 " losses=%" PRIu64
           "\nWDL BTM: wins=%" PRIu64 " draws=%" PRIu64
           " losses=%" PRIu64 "\n",
           statistics.wins[EGTB_WHITE_TO_MOVE],
           statistics.draws[EGTB_WHITE_TO_MOVE],
           statistics.losses[EGTB_WHITE_TO_MOVE],
           statistics.wins[EGTB_BLACK_TO_MOVE],
           statistics.draws[EGTB_BLACK_TO_MOVE],
           statistics.losses[EGTB_BLACK_TO_MOVE]);
    return open_existing(out, path, cache_pages, &not_found);
}

bool wdl_close(Wdl *wdl)
{
    bool ok = true;
    if (wdl == NULL)
        return true;
    if (wdl->file != NULL && fclose(wdl->file) != 0)
        ok = fail("cannot close WDL file: %s", strerror(errno));
    wdl->file = NULL;
    destroy_wdl(wdl);
    return ok;
}

static bool load_page(Wdl *wdl, uint64_t page, WdlCacheEntry *entry)
{
    size_t decompressed;
    uint32_t checksum;
    if (wdl->offsets[page] == 0) {
        memset(entry->data, 0, sizeof(entry->data));
    } else {
        uint16_t length = wdl->lengths[page];
        if (length > wdl->compressed_capacity ||
            !read_at(wdl->file, wdl->offsets[page], wdl->compressed, length))
            return false;
        decompressed = ZSTD_decompressDCtx(
            wdl->decompressor, entry->data, sizeof(entry->data),
            wdl->compressed, length);
        if (ZSTD_isError(decompressed))
            return fail("WDL Zstd decompression failed for page %" PRIu64
                        ": %s", page, ZSTD_getErrorName(decompressed));
        if (decompressed != sizeof(entry->data))
            return fail("decompressed WDL page has an invalid size");
        checksum = crc32c(entry->data, sizeof(entry->data));
        if (checksum != wdl->checksums[page])
            return fail("CRC32C mismatch for WDL page %" PRIu64, page);
    }
    entry->page_number = page;
    return true;
}

bool wdl_get(Wdl *wdl, uint64_t index, EgtbSide side, WdlResult *result)
{
    uint64_t page;
    size_t cache_index;
    uint32_t position_in_page;
    unsigned char packed, value;
    WdlCacheEntry *entry;
    if (wdl == NULL || result == NULL || index > wdl->maximum_index ||
        (side != EGTB_WHITE_TO_MOVE && side != EGTB_BLACK_TO_MOVE))
        return fail("invalid WDL lookup");
    page = index / WDL_POSITIONS_PER_PAGE;
    position_in_page = (uint32_t)(index % WDL_POSITIONS_PER_PAGE);
    cache_index = (size_t)(page % wdl->cache_pages);
    entry = &wdl->cache[cache_index];
    if (entry->page_number != page && !load_page(wdl, page, entry))
        return false;
    packed = entry->data[position_in_page / 2];
    value = (packed >> ((position_in_page % 2) * 4 + side * 2)) & 3;
    if (value == 3)
        return fail("invalid WDL value at index %" PRIu64, index);
    *result = (WdlResult)value;
    return true;
}

uint64_t wdl_maximum_index(const Wdl *wdl)
{
    return wdl == NULL ? 0 : wdl->maximum_index;
}

uint64_t wdl_page_count(const Wdl *wdl)
{
    return wdl == NULL ? 0 : wdl->page_count;
}

uint32_t wdl_page_size(const Wdl *wdl)
{
    return wdl == NULL ? 0 : WDL_PAGE_SIZE;
}

bool wdl_storage_statistics(const Wdl *wdl,
                            WdlStorageStatistics *statistics)
{
    uint64_t page;
    if (wdl == NULL || statistics == NULL)
        return fail("invalid WDL storage-statistics argument");
    memset(statistics, 0, sizeof(*statistics));
    statistics->logical_uncompressed_bytes = wdl->packed_bytes;
    statistics->file_bytes = wdl->file_bytes;
    for (page = 0; page < wdl->page_count; ++page) {
        if (wdl->offsets[page] != 0) {
            ++statistics->stored_pages;
            statistics->compressed_payload_bytes += wdl->lengths[page];
        }
    }
    return true;
}
