#define _POSIX_C_SOURCE 200809L

#include "egtb.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <zstd.h>

#define EGTB_HEADER_SIZE 64
#define EGTB_DIRECTORY_ENTRY_SIZE 10
#define EGTB_BLOCK_HEADER_SIZE 4
#define EGTB_FLAG_EXACT_LAYOUT 1
typedef struct {
    uint64_t page_index;
    uint32_t checksum;
    bool valid;
    bool dirty;
} CacheEntry;

typedef struct {
    CacheEntry *entries;
    unsigned char *data;
    size_t capacity;
} PageCache;

typedef struct {
    uint64_t page_index;
    uint32_t checksum;
    bool valid;
    bool dirty;
} DirectCacheEntry;

struct Egtb {
    FILE *file;
    char *path;
    uint64_t maximum_index;
    uint64_t page_count;
    uint64_t directory_offset;
    uint64_t data_offset;
    uint64_t *offsets;
    uint16_t *lengths;
    uint32_t page_size;
    uint32_t entries_per_page;
    unsigned reserve_percent;
    int compression_level;
    bool readonly;
    bool exact_layout;
    bool registered;
    unsigned references;
    unsigned view_count;
    unsigned writable_views;
    pthread_mutex_t mutex;
    bool mutex_initialized;
    struct EgtbView *views;
    PageCache cache;
    ZSTD_CCtx *compressor;
    ZSTD_DCtx *decompressor;
    unsigned char *compressed;
    size_t compressed_capacity;
    struct Egtb *registry_next;
};

struct EgtbView {
    Egtb *backing;
    DirectCacheEntry *entries;
    unsigned char *data;
    size_t capacity;
    bool writable;
    ZSTD_CCtx *compressor;
    ZSTD_DCtx *decompressor;
    unsigned char *compressed;
    size_t compressed_capacity;
    uint64_t first_page;
    uint64_t end_page;
    struct EgtbView *next;
    EgtbCacheStatistics statistics;
};

static const unsigned char egtb_magic[8] = {'I','P','D','E','G','T','B','\0'};
static Egtb *readonly_registry;
static _Thread_local char last_error[256];
static pthread_mutex_t registry_mutex = PTHREAD_MUTEX_INITIALIZER;

_Static_assert(sizeof(EgtbEntry) == 2, "EgtbEntry must occupy two bytes");

static bool fail(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(last_error, sizeof(last_error), format, arguments);
    va_end(arguments);
    return false;
}

const char *egtb_last_error(void)
{
    return last_error;
}

bool egtb_encode_dtm(int16_t value, int8_t *stored)
{
    if (stored == NULL)
        return false;
    if (value == EGTB_DRAW) {
        *stored = EGTB_STORED_DRAW;
        return true;
    }
    if (value > 0 && value <= EGTB_MAX_WIN_DTM && value % 2 != 0) {
        *stored = (int8_t)((value + 1) / 2);
        return true;
    }
    if (value <= 0 && value >= -EGTB_MAX_LOSS_DTM && value % 2 == 0) {
        *stored = (int8_t)(value / 2);
        return true;
    }
    return false;
}

int16_t egtb_decode_dtm(int8_t stored)
{
    if (stored == EGTB_STORED_DRAW)
        return EGTB_DRAW;
    if (stored >= 1)
        return (int16_t)(2 * (int16_t)stored - 1);
    return (int16_t)(2 * (int16_t)stored);
}

bool egtb_material_filename(char *buffer, size_t buffer_size,
                            unsigned white_kings, unsigned white_men,
                            unsigned black_kings, unsigned black_men,
                            const char *extension)
{
    int length;
    if (buffer == NULL || buffer_size == 0 || extension == NULL ||
        *extension == '\0' || strchr(extension, '/') != NULL)
        return fail("invalid EGTB filename argument");
    length = snprintf(buffer, buffer_size, "%uwX-%uwO-%ubX-%ubO.%s",
                      white_kings, white_men, black_kings, black_men,
                      extension);
    if (length < 0 || (size_t)length >= buffer_size)
        return fail("material filename buffer is too small");
    return true;
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

static bool pread_at(int descriptor, uint64_t offset, void *data, size_t size)
{
    unsigned char *destination = data;
    size_t completed = 0;
    if (offset > (uint64_t)INT64_MAX ||
        size > (size_t)((uint64_t)INT64_MAX - offset))
        return fail("file read offset is too large");
    while (completed < size) {
        ssize_t result = pread(descriptor, destination + completed,
                               size - completed,
                               (off_t)(offset + completed));
        if (result < 0) {
            if (errno == EINTR)
                continue;
            return fail("file read failed: %s", strerror(errno));
        }
        if (result == 0)
            return fail("file read failed or was truncated");
        completed += (size_t)result;
    }
    return true;
}

static bool read_at(FILE *file, uint64_t offset, void *data, size_t size)
{
    int descriptor = fileno(file);
    if (descriptor < 0)
        return fail("cannot obtain EGTB file descriptor");
    return pread_at(descriptor, offset, data, size);
}

static bool write_at(FILE *file, uint64_t offset, const void *data, size_t size)
{
    const unsigned char *source = data;
    size_t completed = 0;
    int descriptor = fileno(file);
    if (descriptor < 0 || offset > (uint64_t)INT64_MAX ||
        size > (size_t)((uint64_t)INT64_MAX - offset))
        return fail("file write offset is too large");
    while (completed < size) {
        ssize_t result = pwrite(descriptor, source + completed,
                                size - completed,
                                (off_t)(offset + completed));
        if (result < 0) {
            if (errno == EINTR)
                continue;
            return fail("file write failed: %s", strerror(errno));
        }
        if (result == 0)
            return fail("file write made no progress");
        completed += (size_t)result;
    }
    return true;
}

static bool write_header(Egtb *egtb)
{
    unsigned char header[EGTB_HEADER_SIZE] = {0};
    memcpy(header, egtb_magic, sizeof(egtb_magic));
    header[8] = EGTB_FORMAT_VERSION;
    header[9] = egtb->exact_layout ? EGTB_FLAG_EXACT_LAYOUT : 0;
    put_u16(header + 10, EGTB_HEADER_SIZE);
    put_u32(header + 12, egtb->page_size);
    put_u64(header + 16, egtb->maximum_index);
    put_u64(header + 24, egtb->page_count);
    put_u64(header + 32, egtb->directory_offset);
    put_u64(header + 40, egtb->data_offset);
    put_u16(header + 48, (uint16_t)egtb->reserve_percent);
    put_u16(header + 50, (uint16_t)(int16_t)egtb->compression_level);
    return write_at(egtb->file, 0, header, sizeof(header));
}

static bool read_header(Egtb *egtb)
{
    unsigned char header[EGTB_HEADER_SIZE];
    uint64_t calculated_pages, entry_count;
    if (!read_at(egtb->file, 0, header, sizeof(header)))
        return false;
    if (memcmp(header, egtb_magic, sizeof(egtb_magic)) != 0)
        return fail("not an International Polish Draughts EGTB");
    if (header[8] != EGTB_FORMAT_VERSION)
        return fail("unsupported EGTB version %u", (unsigned)header[8]);
    if (get_u16(header + 10) != EGTB_HEADER_SIZE)
        return fail("unsupported EGTB header size");
    egtb->exact_layout = (header[9] & EGTB_FLAG_EXACT_LAYOUT) != 0;
    egtb->page_size = get_u32(header + 12);
    egtb->maximum_index = get_u64(header + 16);
    egtb->page_count = get_u64(header + 24);
    egtb->directory_offset = get_u64(header + 32);
    egtb->data_offset = get_u64(header + 40);
    egtb->reserve_percent = get_u16(header + 48);
    egtb->compression_level = (int16_t)get_u16(header + 50);
    if (egtb->page_size == 0 || egtb->page_size % sizeof(EgtbEntry) != 0)
        return fail("invalid EGTB page size");
    egtb->entries_per_page = egtb->page_size / sizeof(EgtbEntry);
    if (egtb->maximum_index == UINT64_MAX)
        return fail("invalid EGTB maximum index");
    entry_count = egtb->maximum_index + 1;
    calculated_pages = entry_count / egtb->entries_per_page +
                       (entry_count % egtb->entries_per_page != 0);
    if (egtb->reserve_percent > 100 ||
        egtb->page_count > (UINT64_MAX - EGTB_HEADER_SIZE) /
                           EGTB_DIRECTORY_ENTRY_SIZE ||
        calculated_pages != egtb->page_count ||
        egtb->directory_offset != EGTB_HEADER_SIZE ||
        egtb->data_offset != EGTB_HEADER_SIZE +
                             egtb->page_count * EGTB_DIRECTORY_ENTRY_SIZE)
        return fail("inconsistent EGTB header");
    return true;
}

static uint32_t mix32(uint32_t value)
{
    value ^= value >> 16;
    value *= UINT32_C(0x7feb352d);
    value ^= value >> 15;
    value *= UINT32_C(0x846ca68b);
    return value ^ (value >> 16);
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

static uint32_t slot_checksum(uint32_t slot, const EgtbEntry *entry)
{
    uint32_t values = (uint8_t)entry->white_to_move |
                      (uint32_t)(uint8_t)entry->black_to_move << 8;
    return mix32(values ^ mix32(slot + UINT32_C(0x9e3779b9)));
}

static uint32_t page_checksum(const Egtb *egtb, const EgtbEntry *entries)
{
    uint32_t checksum = 0;
    uint32_t i;
    for (i = 0; i < egtb->entries_per_page; ++i)
        checksum ^= slot_checksum(i, &entries[i]);
    return checksum;
}

static bool cache_init(Egtb *egtb, size_t capacity)
{
    PageCache *cache = &egtb->cache;
    if (capacity == 0)
        return fail("page cache must contain at least one page");
    if (egtb->page_count < capacity)
        capacity = (size_t)egtb->page_count;
    if (capacity == 0)
        capacity = 1;
    if (capacity > SIZE_MAX / egtb->page_size)
        return fail("page cache byte size overflows size_t");
    cache->entries = calloc(capacity, sizeof(*cache->entries));
    cache->data = malloc(capacity * egtb->page_size);
    if (cache->entries == NULL || cache->data == NULL)
        return fail("cannot allocate page cache");
    cache->capacity = capacity;
    return true;
}

static void cache_destroy(PageCache *cache)
{
    free(cache->entries);
    free(cache->data);
    memset(cache, 0, sizeof(*cache));
}

static void cache_invalidate(PageCache *cache)
{
    memset(cache->entries, 0, cache->capacity * sizeof(*cache->entries));
}

static EgtbEntry *cache_data(Egtb *egtb, size_t index)
{
    return (EgtbEntry *)(void *)(egtb->cache.data + index * egtb->page_size);
}

static bool directory_entry(Egtb *egtb, uint64_t page, uint64_t *offset,
                            uint16_t *length)
{
    *offset = egtb->offsets[page];
    *length = egtb->lengths[page];
    return true;
}

static bool all_draws(const Egtb *egtb, const EgtbEntry *entries)
{
    uint32_t i;
    for (i = 0; i < egtb->entries_per_page; ++i) {
        if (entries[i].white_to_move != EGTB_STORED_DRAW ||
            entries[i].black_to_move != EGTB_STORED_DRAW)
            return false;
    }
    return true;
}

static bool append_block(Egtb *egtb, uint32_t checksum,
                         const void *compressed, uint16_t length,
                         uint16_t capacity, uint64_t *offset)
{
    struct stat status;
    unsigned char header[EGTB_BLOCK_HEADER_SIZE];
    int descriptor = fileno(egtb->file);
    if (descriptor < 0 || fstat(descriptor, &status) != 0)
        return fail("cannot determine EGTB length");
    if (status.st_size < 0)
        return fail("EGTB has a negative file length");
    *offset = (uint64_t)status.st_size;
    put_u32(header, checksum);
    if (!write_at(egtb->file, *offset, header, sizeof(header)) ||
        !write_at(egtb->file, *offset + EGTB_BLOCK_HEADER_SIZE,
                  compressed, length))
        return false;
    if (capacity > length) {
        uint64_t last = *offset + EGTB_BLOCK_HEADER_SIZE + capacity - 1;
        unsigned char zero = 0;
        if (!write_at(egtb->file, last, &zero, 1))
            return false;
    }
    return true;
}

static bool store_page_with_runtime(Egtb *egtb, ZSTD_CCtx *compressor,
                                    unsigned char *compressed,
                                    size_t compressed_capacity,
                                    uint64_t page,
                                    const EgtbEntry *entries, bool exact,
                                    bool synchronize_append)
{
    uint64_t old_offset = egtb->offsets[page];
    uint16_t old_length = egtb->lengths[page];
    size_t compressed_size;
    uint16_t length, minimum, available;
    uint32_t checksum;
    unsigned char checksum_bytes[EGTB_BLOCK_HEADER_SIZE];

    if (all_draws(egtb, entries)) {
        egtb->offsets[page] = 0;
        egtb->lengths[page] = 0;
        return true;
    }
    compressed_size = ZSTD_compressCCtx(compressor, compressed,
                                        compressed_capacity, entries,
                                        egtb->page_size,
                                        egtb->compression_level);
    if (ZSTD_isError(compressed_size))
        return fail("Zstd compression failed: %s",
                    ZSTD_getErrorName(compressed_size));
    if (compressed_size == 0 || compressed_size > UINT16_MAX)
        return fail("compressed page does not fit in 16-bit length");
    length = (uint16_t)compressed_size;
    checksum = crc32c(entries, egtb->page_size);
    put_u32(checksum_bytes, checksum);
    minimum = exact ? length :
        (uint16_t)(((uint64_t)egtb->page_size * egtb->reserve_percent + 99) / 100);
    if (minimum < length)
        minimum = length;

    available = old_length;
    if (!egtb->exact_layout) {
        uint16_t reserve = (uint16_t)(((uint64_t)egtb->page_size *
                                      egtb->reserve_percent + 99) / 100);
        if (available < reserve)
            available = reserve;
    }
    if (old_offset != 0 && length <= available) {
        if (!write_at(egtb->file, old_offset, checksum_bytes,
                      sizeof(checksum_bytes)) ||
            !write_at(egtb->file, old_offset + EGTB_BLOCK_HEADER_SIZE,
                      compressed, length))
            return false;
        egtb->offsets[page] = old_offset;
    } else {
        uint64_t new_offset = 0;
        bool appended;
        if (synchronize_append)
            pthread_mutex_lock(&egtb->mutex);
        appended = append_block(egtb, checksum, compressed, length,
                                minimum, &new_offset);
        if (appended)
            egtb->offsets[page] = new_offset;
        if (synchronize_append)
            pthread_mutex_unlock(&egtb->mutex);
        if (!appended)
            return false;
    }
    egtb->lengths[page] = length;
    return true;
}

static bool store_page(Egtb *egtb, uint64_t page, const EgtbEntry *entries,
                       bool exact)
{
    return store_page_with_runtime(
        egtb, egtb->compressor, egtb->compressed,
        egtb->compressed_capacity, page, entries, exact, false);
}

static bool flush_cache_entry(Egtb *egtb, size_t index)
{
    CacheEntry *entry = &egtb->cache.entries[index];
    EgtbEntry *data;
    if (!entry->valid || !entry->dirty)
        return true;
    if (egtb->readonly)
        return fail("attempt to flush a dirty read-only page");
    data = cache_data(egtb, index);
    if (page_checksum(egtb, data) != entry->checksum)
        return fail("uncompressed page checksum mismatch for page %" PRIu64,
                    entry->page_index);
    if (!store_page(egtb, entry->page_index, data, false))
        return false;
    entry->dirty = false;
    return true;
}

static void fill_draw_page(Egtb *egtb, EgtbEntry *entries)
{
    uint32_t i;
    for (i = 0; i < egtb->entries_per_page; ++i) {
        entries[i].white_to_move = EGTB_STORED_DRAW;
        entries[i].black_to_move = EGTB_STORED_DRAW;
    }
}

static bool load_page(Egtb *egtb, uint64_t page, EgtbEntry *entries)
{
    uint64_t offset;
    uint16_t length;
    size_t decompressed;
    unsigned char checksum_bytes[EGTB_BLOCK_HEADER_SIZE];
    uint32_t expected_checksum, actual_checksum;
    if (!directory_entry(egtb, page, &offset, &length))
        return false;
    if (offset == 0) {
        fill_draw_page(egtb, entries);
        return true;
    }
    if (length > egtb->compressed_capacity)
        return fail("compressed page length exceeds format bound");
    if (!read_at(egtb->file, offset, checksum_bytes, sizeof(checksum_bytes)) ||
        !read_at(egtb->file, offset + EGTB_BLOCK_HEADER_SIZE,
                 egtb->compressed, length))
        return false;
    expected_checksum = get_u32(checksum_bytes);
    decompressed = ZSTD_decompressDCtx(egtb->decompressor, entries,
                                       egtb->page_size, egtb->compressed,
                                       length);
    if (ZSTD_isError(decompressed))
        return fail("Zstd decompression failed for page %" PRIu64 ": %s",
                    page, ZSTD_getErrorName(decompressed));
    if (decompressed != egtb->page_size)
        return fail("decompressed page has an invalid size");
    actual_checksum = crc32c(entries, egtb->page_size);
    if (actual_checksum != expected_checksum)
        return fail("CRC32C mismatch for uncompressed page %" PRIu64, page);
    return true;
}

static bool cached_page(Egtb *egtb, uint64_t page, size_t *result)
{
    PageCache *cache = &egtb->cache;
    size_t index = (size_t)(page % cache->capacity);
    CacheEntry *entry;
    EgtbEntry *data;

    entry = &cache->entries[index];
    if (entry->valid && entry->page_index == page) {
        *result = index;
        return true;
    }
    if (entry->valid && entry->dirty) {
        if (!flush_cache_entry(egtb, index))
            return false;
    }
    data = cache_data(egtb, index);
    if (!load_page(egtb, page, data))
        return false;
    entry->page_index = page;
    entry->checksum = page_checksum(egtb, data);
    entry->dirty = false;
    entry->valid = true;
    *result = index;
    return true;
}

static bool write_directory(Egtb *egtb)
{
    unsigned char buffer[EGTB_DIRECTORY_ENTRY_SIZE * 4096];
    uint64_t page = 0;
    if (egtb->readonly)
        return true;
    while (page < egtb->page_count) {
        size_t count = (size_t)(egtb->page_count - page);
        size_t i;
        if (count > 4096)
            count = 4096;
        for (i = 0; i < count; ++i) {
            put_u64(buffer + i * EGTB_DIRECTORY_ENTRY_SIZE,
                    egtb->offsets[page + i]);
            put_u16(buffer + i * EGTB_DIRECTORY_ENTRY_SIZE + 8,
                    egtb->lengths[page + i]);
        }
        if (!write_at(egtb->file,
                      egtb->directory_offset + page * EGTB_DIRECTORY_ENTRY_SIZE,
                      buffer, count * EGTB_DIRECTORY_ENTRY_SIZE))
            return false;
        page += count;
    }
    return true;
}

static void destroy_egtb(Egtb *egtb)
{
    if (egtb == NULL)
        return;
    if (egtb->file != NULL)
        fclose(egtb->file);
    cache_destroy(&egtb->cache);
    ZSTD_freeCCtx(egtb->compressor);
    ZSTD_freeDCtx(egtb->decompressor);
    free(egtb->compressed);
    free(egtb->offsets);
    free(egtb->lengths);
    free(egtb->path);
    if (egtb->mutex_initialized)
        pthread_mutex_destroy(&egtb->mutex);
    free(egtb);
}

static bool initialize_mutex(Egtb *egtb)
{
    int error = pthread_mutex_init(&egtb->mutex, NULL);
    if (error != 0)
        return fail("cannot initialize EGTB mutex: %s", strerror(error));
    egtb->mutex_initialized = true;
    return true;
}

static bool allocate_runtime(Egtb *egtb, size_t cache_pages)
{
    if (crc32c("123456789", 9) != UINT32_C(0xe3069283))
        return fail("internal CRC32C self-test failed");
    egtb->compressed_capacity = ZSTD_compressBound(egtb->page_size);
    if (egtb->compressed_capacity > UINT16_MAX)
        return fail("page size is too large for 16-bit compressed lengths");
    egtb->compressed = malloc(egtb->compressed_capacity);
    egtb->compressor = ZSTD_createCCtx();
    egtb->decompressor = ZSTD_createDCtx();
    if (egtb->compressed == NULL || egtb->compressor == NULL ||
        egtb->decompressor == NULL)
        return fail("cannot allocate Zstd context or buffer");
    return cache_init(egtb, cache_pages);
}

static bool load_directory(Egtb *egtb)
{
    unsigned char buffer[EGTB_DIRECTORY_ENTRY_SIZE * 4096];
    uint64_t page = 0;
    if (egtb->page_count > SIZE_MAX / sizeof(*egtb->offsets) ||
        egtb->page_count > SIZE_MAX / sizeof(*egtb->lengths))
        return fail("directory is too large for this process");
    egtb->offsets = malloc((size_t)egtb->page_count * sizeof(*egtb->offsets));
    egtb->lengths = malloc((size_t)egtb->page_count * sizeof(*egtb->lengths));
    if (egtb->offsets == NULL || egtb->lengths == NULL)
        return fail("cannot allocate read/write directory");
    while (page < egtb->page_count) {
        size_t count = (size_t)(egtb->page_count - page);
        size_t i;
        if (count > 4096)
            count = 4096;
        if (!read_at(egtb->file,
                     egtb->directory_offset + page * EGTB_DIRECTORY_ENTRY_SIZE,
                     buffer, count * EGTB_DIRECTORY_ENTRY_SIZE))
            return false;
        for (i = 0; i < count; ++i) {
            egtb->offsets[page + i] =
                get_u64(buffer + i * EGTB_DIRECTORY_ENTRY_SIZE);
            egtb->lengths[page + i] =
                get_u16(buffer + i * EGTB_DIRECTORY_ENTRY_SIZE + 8);
            if ((egtb->offsets[page + i] == 0) !=
                (egtb->lengths[page + i] == 0))
                return fail("invalid directory entry for page %" PRIu64,
                            page + i);
        }
        page += count;
    }
    return true;
}

static bool open_common(Egtb **out, const char *path, bool readonly,
                        size_t cache_pages)
{
    Egtb *egtb;
    FILE *file;
    *out = NULL;
    file = fopen(path, readonly ? "rb" : "r+b");
    if (file == NULL)
        return fail("cannot open %s: %s", path, strerror(errno));
    egtb = calloc(1, sizeof(*egtb));
    if (egtb == NULL) {
        fclose(file);
        return fail("cannot allocate EGTB handle");
    }
    if (!initialize_mutex(egtb)) {
        fclose(file);
        free(egtb);
        return false;
    }
    egtb->file = file;
    egtb->readonly = readonly;
    egtb->references = 1;
    egtb->path = malloc(strlen(path) + 1);
    if (egtb->path == NULL) {
        destroy_egtb(egtb);
        return fail("cannot allocate EGTB path");
    }
    strcpy(egtb->path, path);
    if (!read_header(egtb) || !load_directory(egtb) ||
        !allocate_runtime(egtb, cache_pages)) {
        destroy_egtb(egtb);
        return false;
    }
    *out = egtb;
    return true;
}

bool egtb_create(Egtb **out, const char *path, uint64_t maximum_index,
                 uint32_t page_size, const EgtbCreateOptions *options)
{
    EgtbCreateOptions defaults = {1024, 20, 3};
    Egtb *egtb;
    uint64_t entries, directory_bytes;
    int descriptor;

    if (out == NULL)
        return fail("invalid EGTB creation output pointer");
    *out = NULL;
    if (options == NULL)
        options = &defaults;
    if (path == NULL || page_size == 0 ||
        page_size % sizeof(EgtbEntry) != 0 || options->cache_pages == 0 ||
        options->reserve_percent > 100 || maximum_index == UINT64_MAX)
        return fail("invalid EGTB creation argument");
    if (ZSTD_compressBound(page_size) > UINT16_MAX)
        return fail("page size is too large for the EGTB format");

    egtb = calloc(1, sizeof(*egtb));
    if (egtb == NULL)
        return fail("cannot allocate EGTB handle");
    if (!initialize_mutex(egtb)) {
        free(egtb);
        return false;
    }
    egtb->page_size = page_size;
    egtb->entries_per_page = page_size / sizeof(EgtbEntry);
    egtb->maximum_index = maximum_index;
    entries = maximum_index + 1;
    egtb->page_count = entries / egtb->entries_per_page +
                       (entries % egtb->entries_per_page != 0);
    if (egtb->page_count > (UINT64_MAX - EGTB_HEADER_SIZE) /
                           EGTB_DIRECTORY_ENTRY_SIZE) {
        destroy_egtb(egtb);
        return fail("EGTB directory size overflows");
    }
    directory_bytes = egtb->page_count * EGTB_DIRECTORY_ENTRY_SIZE;
    egtb->directory_offset = EGTB_HEADER_SIZE;
    egtb->data_offset = EGTB_HEADER_SIZE + directory_bytes;
    if (egtb->data_offset > (uint64_t)INT64_MAX ||
        egtb->page_count > SIZE_MAX / sizeof(*egtb->offsets) ||
        egtb->page_count > SIZE_MAX / sizeof(*egtb->lengths)) {
        destroy_egtb(egtb);
        return fail("EGTB file is too large for this platform");
    }
    egtb->reserve_percent = options->reserve_percent;
    egtb->compression_level = options->compression_level;
    egtb->references = 1;
    egtb->path = malloc(strlen(path) + 1);
    egtb->offsets = calloc((size_t)egtb->page_count, sizeof(*egtb->offsets));
    egtb->lengths = calloc((size_t)egtb->page_count, sizeof(*egtb->lengths));
    if (egtb->path == NULL || egtb->offsets == NULL || egtb->lengths == NULL) {
        destroy_egtb(egtb);
        return fail("cannot allocate EGTB directory");
    }
    strcpy(egtb->path, path);
    descriptor = open(path, O_RDWR | O_CREAT | O_EXCL, 0666);
    if (descriptor < 0) {
        destroy_egtb(egtb);
        return fail("cannot create %s: %s", path, strerror(errno));
    }
    egtb->file = fdopen(descriptor, "w+b");
    if (egtb->file == NULL) {
        close(descriptor);
        destroy_egtb(egtb);
        return fail("cannot create file stream: %s", strerror(errno));
    }
    if (!write_header(egtb) || ftruncate(descriptor, (off_t)egtb->data_offset) != 0 ||
        !allocate_runtime(egtb, options->cache_pages)) {
        destroy_egtb(egtb);
        unlink(path);
        return fail("cannot initialize EGTB file");
    }
    *out = egtb;
    return true;
}

bool egtb_open_readonly(Egtb **out, const char *path, size_t cache_pages)
{
    Egtb *current;
    if (out == NULL || path == NULL)
        return fail("invalid read-only open argument");
    pthread_mutex_lock(&registry_mutex);
    for (current = readonly_registry; current != NULL;
         current = current->registry_next) {
        if (strcmp(current->path, path) == 0) {
            ++current->references;
            *out = current;
            pthread_mutex_unlock(&registry_mutex);
            return true;
        }
    }
    if (!open_common(out, path, true, cache_pages)) {
        pthread_mutex_unlock(&registry_mutex);
        return false;
    }
    (*out)->registered = true;
    (*out)->registry_next = readonly_registry;
    readonly_registry = *out;
    pthread_mutex_unlock(&registry_mutex);
    return true;
}

bool egtb_open_readwrite(Egtb **out, const char *path, size_t cache_pages)
{
    Egtb *current;
    if (out == NULL || path == NULL)
        return fail("invalid read/write open argument");
    pthread_mutex_lock(&registry_mutex);
    for (current = readonly_registry; current != NULL;
         current = current->registry_next) {
        if (strcmp(current->path, path) == 0) {
            pthread_mutex_unlock(&registry_mutex);
            return fail("EGTB is already open read-only");
        }
    }
    pthread_mutex_unlock(&registry_mutex);
    return open_common(out, path, false, cache_pages);
}

bool egtb_flush(Egtb *egtb)
{
    size_t i;
    if (egtb == NULL)
        return fail("cannot flush a null EGTB");
    if (egtb->writable_views != 0)
        return fail("cannot flush an EGTB with an active writable view");
    if (egtb->readonly)
        return true;
    for (i = 0; i < egtb->cache.capacity; ++i) {
        if (!flush_cache_entry(egtb, i))
            return false;
    }
    if (!write_header(egtb) || !write_directory(egtb) ||
        fflush(egtb->file) != 0)
        return fail("cannot flush EGTB file");
    return true;
}

bool egtb_close(Egtb *egtb)
{
    bool ok = true;
    if (egtb == NULL)
        return true;
    if (egtb->view_count != 0)
        return fail("cannot close an EGTB with active cache views");
    if (egtb->registered) {
        Egtb **link = &readonly_registry;
        pthread_mutex_lock(&registry_mutex);
        if (egtb->references > 1) {
            --egtb->references;
            pthread_mutex_unlock(&registry_mutex);
            return true;
        }
        while (*link != egtb)
            link = &(*link)->registry_next;
        *link = egtb->registry_next;
        pthread_mutex_unlock(&registry_mutex);
    }
    if (!egtb->readonly)
        ok = egtb_flush(egtb);
    if (egtb->file != NULL && fclose(egtb->file) != 0)
        ok = fail("cannot close EGTB file");
    egtb->file = NULL;
    destroy_egtb(egtb);
    return ok;
}

bool egtb_get(Egtb *egtb, uint64_t index, EgtbSide side, int16_t *value)
{
    uint64_t page;
    uint32_t slot;
    size_t cache_index;
    EgtbEntry *entries;
    if (egtb == NULL || egtb->writable_views != 0 || value == NULL ||
        index > egtb->maximum_index ||
        (side != EGTB_WHITE_TO_MOVE && side != EGTB_BLACK_TO_MOVE))
        return fail("invalid EGTB lookup");
    page = index / egtb->entries_per_page;
    slot = (uint32_t)(index % egtb->entries_per_page);
    if (!cached_page(egtb, page, &cache_index))
        return false;
    entries = cache_data(egtb, cache_index);
    *value = egtb_decode_dtm(side == EGTB_WHITE_TO_MOVE
                                ? entries[slot].white_to_move
                                : entries[slot].black_to_move);
    return true;
}

bool egtb_set(Egtb *egtb, uint64_t index, EgtbSide side, int16_t value)
{
    uint64_t page;
    uint32_t slot;
    size_t cache_index;
    CacheEntry *cache_entry;
    EgtbEntry *entries;
    EgtbEntry old;
    int8_t stored;
    if (egtb == NULL || egtb->readonly || egtb->writable_views != 0 ||
        index > egtb->maximum_index ||
        !egtb_encode_dtm(value, &stored) ||
        (side != EGTB_WHITE_TO_MOVE && side != EGTB_BLACK_TO_MOVE))
        return fail("invalid EGTB update");
    page = index / egtb->entries_per_page;
    slot = (uint32_t)(index % egtb->entries_per_page);
    if (!cached_page(egtb, page, &cache_index))
        return false;
    cache_entry = &egtb->cache.entries[cache_index];
    entries = cache_data(egtb, cache_index);
    old = entries[slot];
    if (side == EGTB_WHITE_TO_MOVE)
        entries[slot].white_to_move = stored;
    else
        entries[slot].black_to_move = stored;
    cache_entry->checksum ^= slot_checksum(slot, &old) ^
                             slot_checksum(slot, &entries[slot]);
    cache_entry->dirty = true;
    return true;
}

static EgtbEntry *view_cache_data(EgtbView *view, size_t slot)
{
    return (EgtbEntry *)(void *)(view->data +
                                 slot * view->backing->page_size);
}

static bool view_load_page(EgtbView *view, uint64_t page, EgtbEntry *entries)
{
    Egtb *egtb = view->backing;
    uint64_t offset = egtb->offsets[page];
    uint16_t length = egtb->lengths[page];
    size_t decompressed;
    unsigned char checksum_bytes[EGTB_BLOCK_HEADER_SIZE];
    uint32_t expected_checksum, actual_checksum;
    int descriptor = fileno(egtb->file);
    if (offset == 0) {
        fill_draw_page(egtb, entries);
        return true;
    }
    if (descriptor < 0 || length > view->compressed_capacity)
        return fail("cannot read compressed page through cache view");
    if (!pread_at(descriptor, offset, checksum_bytes,
                  sizeof(checksum_bytes)) ||
        !pread_at(descriptor, offset + EGTB_BLOCK_HEADER_SIZE,
                  view->compressed, length))
        return false;
    expected_checksum = get_u32(checksum_bytes);
    decompressed = ZSTD_decompressDCtx(
        view->decompressor, entries, egtb->page_size,
        view->compressed, length);
    if (ZSTD_isError(decompressed))
        return fail("Zstd decompression failed for page %" PRIu64 ": %s",
                    page, ZSTD_getErrorName(decompressed));
    if (decompressed != egtb->page_size)
        return fail("decompressed page has an invalid size");
    actual_checksum = crc32c(entries, egtb->page_size);
    if (actual_checksum != expected_checksum)
        return fail("CRC32C mismatch for uncompressed page %" PRIu64, page);
    ++view->statistics.decompressions;
    return true;
}

static bool view_flush_slot(EgtbView *view, size_t slot)
{
    DirectCacheEntry *entry = &view->entries[slot];
    EgtbEntry *data;
    bool compressed_write;
    if (!entry->valid || !entry->dirty)
        return true;
    if (!view->writable)
        return fail("attempt to flush a dirty read-only cache view");
    data = view_cache_data(view, slot);
    if (page_checksum(view->backing, data) != entry->checksum)
        return fail("uncompressed page checksum mismatch for page %" PRIu64,
                    entry->page_index);
    compressed_write = !all_draws(view->backing, data);
    if (!store_page_with_runtime(
            view->backing, view->compressor, view->compressed,
            view->compressed_capacity, entry->page_index, data, false,
            true)) {
        return false;
    }
    if (compressed_write)
        ++view->statistics.compressed_writes;
    entry->dirty = false;
    return true;
}

static bool view_cached_page(EgtbView *view, uint64_t page, size_t *result)
{
    size_t slot = (size_t)(page % view->capacity);
    DirectCacheEntry *entry = &view->entries[slot];
    EgtbEntry *data = view_cache_data(view, slot);
    ++view->statistics.lookups;
    if (entry->valid && entry->page_index == page) {
        ++view->statistics.hits;
        *result = slot;
        return true;
    }
    ++view->statistics.misses;
    if (entry->valid && entry->dirty) {
        ++view->statistics.dirty_evictions;
        if (!view_flush_slot(view, slot))
            return false;
    }
    if (!view_load_page(view, page, data))
        return false;
    entry->page_index = page;
    entry->checksum = page_checksum(view->backing, data);
    entry->valid = true;
    entry->dirty = false;
    *result = slot;
    return true;
}

bool egtb_view_create(EgtbView **out, Egtb *backing, size_t cache_pages,
                      bool writable)
{
    return egtb_view_create_range(out, backing, cache_pages, writable, 0,
                                  backing == NULL ? 0 : backing->page_count);
}

bool egtb_view_create_range(EgtbView **out, Egtb *backing,
                            size_t cache_pages, bool writable,
                            uint64_t first_page, uint64_t end_page)
{
    EgtbView *view;
    EgtbView *other;
    uint64_t range_pages;
    if (out == NULL)
        return fail("invalid cache-view output pointer");
    *out = NULL;
    if (backing == NULL || cache_pages == 0 ||
        (writable && backing->readonly) ||
        first_page >= end_page || end_page > backing->page_count)
        return fail("invalid or conflicting cache-view request");
    if (writable && backing->writable_views == 0 && !egtb_flush(backing))
        return false;
    pthread_mutex_lock(&backing->mutex);
    for (other = backing->views; other != NULL; other = other->next) {
        if ((writable || other->writable) &&
            first_page < other->end_page && end_page > other->first_page) {
            pthread_mutex_unlock(&backing->mutex);
            return fail("cache-view page ranges overlap");
        }
    }
    pthread_mutex_unlock(&backing->mutex);
    range_pages = end_page - first_page;
    if (range_pages < cache_pages)
        cache_pages = (size_t)range_pages;
    if (cache_pages == 0)
        cache_pages = 1;
    if (cache_pages > SIZE_MAX / backing->page_size)
        return fail("cache-view byte size overflows size_t");
    view = calloc(1, sizeof(*view));
    if (view == NULL)
        return fail("cannot allocate cache view");
    view->backing = backing;
    view->capacity = cache_pages;
    view->writable = writable;
    view->first_page = first_page;
    view->end_page = end_page;
    view->compressed_capacity = ZSTD_compressBound(backing->page_size);
    view->entries = calloc(cache_pages, sizeof(*view->entries));
    view->data = malloc(cache_pages * backing->page_size);
    view->compressed = malloc(view->compressed_capacity);
    view->decompressor = ZSTD_createDCtx();
    if (writable)
        view->compressor = ZSTD_createCCtx();
    if (view->entries == NULL || view->data == NULL ||
        view->compressed == NULL || view->decompressor == NULL ||
        (writable && view->compressor == NULL)) {
        ZSTD_freeCCtx(view->compressor);
        ZSTD_freeDCtx(view->decompressor);
        free(view->compressed);
        free(view->data);
        free(view->entries);
        free(view);
        return fail("cannot allocate direct-mapped cache view");
    }
    pthread_mutex_lock(&backing->mutex);
    view->next = backing->views;
    backing->views = view;
    ++backing->view_count;
    if (writable)
        ++backing->writable_views;
    pthread_mutex_unlock(&backing->mutex);
    *out = view;
    return true;
}

bool egtb_view_flush(EgtbView *view)
{
    size_t slot;
    Egtb *egtb;
    if (view == NULL)
        return fail("cannot flush a null cache view");
    if (!view->writable)
        return true;
    egtb = view->backing;
    for (slot = 0; slot < view->capacity; ++slot) {
        if (!view_flush_slot(view, slot))
            return false;
    }
    pthread_mutex_lock(&egtb->mutex);
    if (!write_header(egtb) || !write_directory(egtb) ||
        fflush(egtb->file) != 0) {
        pthread_mutex_unlock(&egtb->mutex);
        return fail("cannot flush EGTB cache view");
    }
    pthread_mutex_unlock(&egtb->mutex);
    return true;
}

bool egtb_view_close(EgtbView *view)
{
    bool ok = true;
    Egtb *backing;
    if (view == NULL)
        return true;
    backing = view->backing;
    if (backing != NULL && view->writable)
        ok = egtb_view_flush(view);
    if (backing != NULL) {
        EgtbView **link;
        pthread_mutex_lock(&backing->mutex);
        link = &backing->views;
        while (*link != NULL && *link != view)
            link = &(*link)->next;
        if (*link == view)
            *link = view->next;
        if (backing->view_count != 0)
            --backing->view_count;
        if (view->writable && backing->writable_views != 0)
            --backing->writable_views;
        if (backing->writable_views == 0)
            cache_invalidate(&backing->cache);
        pthread_mutex_unlock(&backing->mutex);
    }
    ZSTD_freeCCtx(view->compressor);
    ZSTD_freeDCtx(view->decompressor);
    free(view->compressed);
    free(view->data);
    free(view->entries);
    free(view);
    return ok;
}

bool egtb_view_get(EgtbView *view, uint64_t index, EgtbSide side,
                   int16_t *value)
{
    Egtb *egtb;
    uint64_t page;
    uint32_t entry_index;
    size_t cache_slot;
    EgtbEntry *entries;
    if (view == NULL || value == NULL)
        return fail("invalid cache-view lookup");
    egtb = view->backing;
    if (index > egtb->maximum_index ||
        (side != EGTB_WHITE_TO_MOVE && side != EGTB_BLACK_TO_MOVE))
        return fail("invalid cache-view lookup");
    page = index / egtb->entries_per_page;
    if (page < view->first_page || page >= view->end_page)
        return fail("cache-view lookup is outside its page range");
    entry_index = (uint32_t)(index % egtb->entries_per_page);
    if (!view_cached_page(view, page, &cache_slot))
        return false;
    entries = view_cache_data(view, cache_slot);
    *value = egtb_decode_dtm(side == EGTB_WHITE_TO_MOVE
                                ? entries[entry_index].white_to_move
                                : entries[entry_index].black_to_move);
    return true;
}

bool egtb_view_set(EgtbView *view, uint64_t index, EgtbSide side,
                   int16_t value)
{
    Egtb *egtb;
    uint64_t page;
    uint32_t entry_index;
    size_t cache_slot;
    DirectCacheEntry *cache_entry;
    EgtbEntry *entries;
    EgtbEntry old;
    int8_t stored;
    if (view == NULL || !view->writable ||
        !egtb_encode_dtm(value, &stored))
        return fail("invalid cache-view update");
    egtb = view->backing;
    if (index > egtb->maximum_index ||
        (side != EGTB_WHITE_TO_MOVE && side != EGTB_BLACK_TO_MOVE))
        return fail("invalid cache-view update");
    page = index / egtb->entries_per_page;
    if (page < view->first_page || page >= view->end_page)
        return fail("cache-view update is outside its page range");
    entry_index = (uint32_t)(index % egtb->entries_per_page);
    if (!view_cached_page(view, page, &cache_slot))
        return false;
    cache_entry = &view->entries[cache_slot];
    entries = view_cache_data(view, cache_slot);
    old = entries[entry_index];
    if (side == EGTB_WHITE_TO_MOVE)
        entries[entry_index].white_to_move = stored;
    else
        entries[entry_index].black_to_move = stored;
    cache_entry->checksum ^= slot_checksum(entry_index, &old) ^
                             slot_checksum(entry_index,
                                           &entries[entry_index]);
    cache_entry->dirty = true;
    return true;
}

void egtb_view_cache_statistics(const EgtbView *view,
                                EgtbCacheStatistics *statistics)
{
    if (statistics == NULL)
        return;
    if (view == NULL)
        memset(statistics, 0, sizeof(*statistics));
    else
        *statistics = view->statistics;
}

uint64_t egtb_maximum_index(const Egtb *egtb)
{
    return egtb == NULL ? 0 : egtb->maximum_index;
}

uint64_t egtb_page_count(const Egtb *egtb)
{
    return egtb == NULL ? 0 : egtb->page_count;
}

uint32_t egtb_page_size(const Egtb *egtb)
{
    return egtb == NULL ? 0 : egtb->page_size;
}

bool egtb_is_readonly(const Egtb *egtb)
{
    return egtb != NULL && egtb->readonly;
}

unsigned egtb_reserve_percent(const Egtb *egtb)
{
    return egtb == NULL ? 0 : egtb->reserve_percent;
}

size_t egtb_cache_pages(const Egtb *egtb)
{
    return egtb == NULL ? 0 : egtb->cache.capacity;
}

bool egtb_resize_cache(Egtb *egtb, size_t cache_pages)
{
    if (egtb == NULL || cache_pages == 0 || egtb->view_count != 0)
        return fail("invalid EGTB cache resize");
    if (!egtb_flush(egtb))
        return false;
    if (egtb->page_count < cache_pages)
        cache_pages = (size_t)egtb->page_count;
    cache_destroy(&egtb->cache);
    return cache_init(egtb, cache_pages);
}

bool egtb_storage_statistics(Egtb *egtb, EgtbStorageStatistics *statistics)
{
    uint64_t page = 0;
    off_t end;

    if (egtb == NULL || statistics == NULL)
        return fail("invalid EGTB storage-statistics argument");
    memset(statistics, 0, sizeof(*statistics));
    if (egtb->maximum_index + 1 > UINT64_MAX / sizeof(EgtbEntry))
        return fail("logical EGTB size overflows uint64_t");
    statistics->logical_uncompressed_bytes =
        (egtb->maximum_index + 1) * sizeof(EgtbEntry);

    if (!egtb->readonly) {
        for (page = 0; page < egtb->page_count; ++page) {
            if (egtb->offsets[page] != 0) {
                ++statistics->live_pages;
                statistics->compressed_payload_bytes += egtb->lengths[page];
            }
        }
    } else {
        unsigned char buffer[EGTB_DIRECTORY_ENTRY_SIZE * 4096];
        while (page < egtb->page_count) {
            size_t count = (size_t)(egtb->page_count - page);
            size_t i;
            if (count > 4096)
                count = 4096;
            if (!read_at(egtb->file,
                         egtb->directory_offset +
                             page * EGTB_DIRECTORY_ENTRY_SIZE,
                         buffer, count * EGTB_DIRECTORY_ENTRY_SIZE))
                return false;
            for (i = 0; i < count; ++i) {
                uint64_t offset =
                    get_u64(buffer + i * EGTB_DIRECTORY_ENTRY_SIZE);
                uint16_t length =
                    get_u16(buffer + i * EGTB_DIRECTORY_ENTRY_SIZE + 8);
                if ((offset == 0) != (length == 0))
                    return fail("invalid directory entry for page %" PRIu64,
                                page + i);
                if (offset != 0) {
                    ++statistics->live_pages;
                    statistics->compressed_payload_bytes += length;
                }
            }
            page += count;
        }
    }
    if (statistics->live_pages >
        (UINT64_MAX - statistics->compressed_payload_bytes) /
            EGTB_BLOCK_HEADER_SIZE)
        return fail("live EGTB block size overflows uint64_t");
    statistics->live_block_bytes = statistics->compressed_payload_bytes +
                                   statistics->live_pages *
                                       EGTB_BLOCK_HEADER_SIZE;
    if (fseeko(egtb->file, 0, SEEK_END) != 0 || (end = ftello(egtb->file)) < 0)
        return fail("cannot determine EGTB file size");
    statistics->file_bytes = (uint64_t)end;
    return true;
}

static bool registry_contains(const char *path)
{
    Egtb *current;
    for (current = readonly_registry; current != NULL;
         current = current->registry_next) {
        if (strcmp(current->path, path) == 0)
            return true;
    }
    return false;
}

bool egtb_compact(const char *path, int compression_level,
                  size_t source_cache_pages)
{
    Egtb *source = NULL, *target = NULL;
    EgtbCreateOptions options;
    char *temporary = NULL;
    size_t path_length;
    int descriptor = -1;
    bool ok = false;
    uint64_t page;

    if (path == NULL || source_cache_pages == 0 || registry_contains(path))
        return fail("cannot compact an open or invalid EGTB");
    if (!open_common(&source, path, true, source_cache_pages))
        goto done;
    path_length = strlen(path);
    temporary = malloc(path_length + 24);
    if (temporary == NULL) {
        fail("cannot allocate compaction path");
        goto done;
    }
    snprintf(temporary, path_length + 24, "%s.compact.XXXXXX", path);
    descriptor = mkstemp(temporary);
    if (descriptor < 0) {
        fail("cannot create compaction file: %s", strerror(errno));
        goto done;
    }
    close(descriptor);
    descriptor = -1;
    unlink(temporary);
    options.cache_pages = 1;
    options.reserve_percent = 0;
    options.compression_level = compression_level;
    if (!egtb_create(&target, temporary, source->maximum_index,
                     source->page_size, &options))
        goto done;
    target->exact_layout = true;
    for (page = 0; page < source->page_count; ++page) {
        size_t cache_index;
        if (!cached_page(source, page, &cache_index) ||
            !store_page(target, page, cache_data(source, cache_index), true))
            goto done;
    }
    if (!egtb_close(target)) {
        target = NULL;
        goto done;
    }
    target = NULL;
    destroy_egtb(source);
    source = NULL;
    if (rename(temporary, path) != 0) {
        fail("cannot replace compacted EGTB: %s", strerror(errno));
        goto done;
    }
    ok = true;

done:
    if (target != NULL)
        egtb_close(target);
    if (source != NULL)
        destroy_egtb(source);
    if (!ok && temporary != NULL)
        unlink(temporary);
    free(temporary);
    return ok;
}
