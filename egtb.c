#define _POSIX_C_SOURCE 200809L

#include "egtb.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <zstd.h>

#define EGTB_HEADER_SIZE 64
#define EGTB_DIRECTORY_ENTRY_SIZE 10
#define EGTB_BLOCK_HEADER_SIZE 4
#define EGTB_FLAG_EXACT_LAYOUT 1
#define CACHE_NONE SIZE_MAX

typedef struct {
    uint64_t page_index;
    uint32_t checksum;
    size_t hash_next;
    size_t lru_previous;
    size_t lru_next;
    bool valid;
    bool dirty;
} CacheEntry;

typedef struct {
    CacheEntry *entries;
    unsigned char *data;
    size_t *buckets;
    size_t capacity;
    size_t used;
    size_t bucket_count;
    size_t lru_head;
    size_t lru_tail;
} PageCache;

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
    PageCache cache;
    ZSTD_CCtx *compressor;
    ZSTD_DCtx *decompressor;
    unsigned char *compressed;
    size_t compressed_capacity;
    struct Egtb *registry_next;
};

static const unsigned char egtb_magic[8] = {'I','P','D','E','G','T','B','\0'};
static Egtb *readonly_registry;
static char last_error[256];

_Static_assert(sizeof(EgtbEntry) == 4, "EgtbEntry must occupy four bytes");

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

bool egtb_material_filename(char *buffer, size_t buffer_size,
                            unsigned white_men, unsigned black_men,
                            unsigned white_kings, unsigned black_kings,
                            const char *extension)
{
    int length;
    if (buffer == NULL || buffer_size == 0 || extension == NULL ||
        *extension == '\0' || strchr(extension, '/') != NULL)
        return fail("invalid EGTB filename argument");
    length = snprintf(buffer, buffer_size, "%uwO-%ubO-%uwX-%ubX.%s",
                      white_men, black_men, white_kings, black_kings,
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

static bool seek_file(FILE *file, uint64_t offset)
{
    if (offset > (uint64_t)INT64_MAX)
        return fail("file offset is too large");
    if (fseeko(file, (off_t)offset, SEEK_SET) != 0)
        return fail("file seek failed: %s", strerror(errno));
    return true;
}

static bool read_at(FILE *file, uint64_t offset, void *data, size_t size)
{
    return seek_file(file, offset) &&
           (fread(data, 1, size, file) == size ||
            fail("file read failed or was truncated"));
}

static bool write_at(FILE *file, uint64_t offset, const void *data, size_t size)
{
    return seek_file(file, offset) &&
           (fwrite(data, 1, size, file) == size || fail("file write failed"));
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
    uint32_t values = (uint16_t)entry->white_to_move |
                      (uint32_t)(uint16_t)entry->black_to_move << 16;
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
    size_t buckets = 1;
    if (capacity == 0)
        return fail("page cache must contain at least one page");
    if (egtb->page_count < capacity)
        capacity = (size_t)egtb->page_count;
    if (capacity == 0)
        capacity = 1;
    while (buckets < capacity * 2) {
        if (buckets > SIZE_MAX / 2)
            return fail("page cache is too large");
        buckets *= 2;
    }
    if (capacity > SIZE_MAX / egtb->page_size)
        return fail("page cache byte size overflows size_t");
    cache->entries = calloc(capacity, sizeof(*cache->entries));
    cache->data = malloc(capacity * egtb->page_size);
    cache->buckets = malloc(buckets * sizeof(*cache->buckets));
    if (cache->entries == NULL || cache->data == NULL || cache->buckets == NULL)
        return fail("cannot allocate page cache");
    cache->capacity = capacity;
    cache->bucket_count = buckets;
    cache->lru_head = CACHE_NONE;
    cache->lru_tail = CACHE_NONE;
    memset(cache->buckets, 0xff, buckets * sizeof(*cache->buckets));
    return true;
}

static void cache_destroy(PageCache *cache)
{
    free(cache->entries);
    free(cache->data);
    free(cache->buckets);
    memset(cache, 0, sizeof(*cache));
}

static size_t hash_bucket(const PageCache *cache, uint64_t page_index)
{
    uint64_t hash = page_index * UINT64_C(11400714819323198485);
    return (size_t)(hash & (cache->bucket_count - 1));
}

static size_t cache_find(const PageCache *cache, uint64_t page_index)
{
    size_t entry = cache->buckets[hash_bucket(cache, page_index)];
    while (entry != CACHE_NONE) {
        if (cache->entries[entry].page_index == page_index)
            return entry;
        entry = cache->entries[entry].hash_next;
    }
    return CACHE_NONE;
}

static void lru_remove(PageCache *cache, size_t index)
{
    CacheEntry *entry = &cache->entries[index];
    if (entry->lru_previous != CACHE_NONE)
        cache->entries[entry->lru_previous].lru_next = entry->lru_next;
    else
        cache->lru_head = entry->lru_next;
    if (entry->lru_next != CACHE_NONE)
        cache->entries[entry->lru_next].lru_previous = entry->lru_previous;
    else
        cache->lru_tail = entry->lru_previous;
}

static void lru_push_front(PageCache *cache, size_t index)
{
    CacheEntry *entry = &cache->entries[index];
    entry->lru_previous = CACHE_NONE;
    entry->lru_next = cache->lru_head;
    if (cache->lru_head != CACHE_NONE)
        cache->entries[cache->lru_head].lru_previous = index;
    else
        cache->lru_tail = index;
    cache->lru_head = index;
}

static void cache_touch(PageCache *cache, size_t index)
{
    if (cache->lru_head == index)
        return;
    lru_remove(cache, index);
    lru_push_front(cache, index);
}

static void hash_remove(PageCache *cache, size_t index)
{
    size_t bucket = hash_bucket(cache, cache->entries[index].page_index);
    size_t *link = &cache->buckets[bucket];
    while (*link != index)
        link = &cache->entries[*link].hash_next;
    *link = cache->entries[index].hash_next;
}

static void hash_insert(PageCache *cache, size_t index)
{
    size_t bucket = hash_bucket(cache, cache->entries[index].page_index);
    cache->entries[index].hash_next = cache->buckets[bucket];
    cache->buckets[bucket] = index;
}

static EgtbEntry *cache_data(Egtb *egtb, size_t index)
{
    return (EgtbEntry *)(void *)(egtb->cache.data + index * egtb->page_size);
}

static bool directory_entry(Egtb *egtb, uint64_t page, uint64_t *offset,
                            uint16_t *length)
{
    if (!egtb->readonly) {
        *offset = egtb->offsets[page];
        *length = egtb->lengths[page];
        return true;
    } else {
        unsigned char bytes[EGTB_DIRECTORY_ENTRY_SIZE];
        uint64_t at = egtb->directory_offset +
                      page * EGTB_DIRECTORY_ENTRY_SIZE;
        if (!read_at(egtb->file, at, bytes, sizeof(bytes)))
            return false;
        *offset = get_u64(bytes);
        *length = get_u16(bytes + 8);
        if ((*offset == 0) != (*length == 0))
            return fail("invalid directory entry for page %" PRIu64, page);
        return true;
    }
}

static bool all_draws(const Egtb *egtb, const EgtbEntry *entries)
{
    uint32_t i;
    for (i = 0; i < egtb->entries_per_page; ++i) {
        if (entries[i].white_to_move != EGTB_DRAW ||
            entries[i].black_to_move != EGTB_DRAW)
            return false;
    }
    return true;
}

static bool append_block(Egtb *egtb, uint32_t checksum,
                         const void *compressed, uint16_t length,
                         uint16_t capacity, uint64_t *offset)
{
    off_t end;
    unsigned char header[EGTB_BLOCK_HEADER_SIZE];
    if (fseeko(egtb->file, 0, SEEK_END) != 0)
        return fail("cannot seek to end of EGTB: %s", strerror(errno));
    end = ftello(egtb->file);
    if (end < 0)
        return fail("cannot determine EGTB length");
    *offset = (uint64_t)end;
    put_u32(header, checksum);
    if (fwrite(header, 1, sizeof(header), egtb->file) != sizeof(header) ||
        fwrite(compressed, 1, length, egtb->file) != length)
        return fail("cannot append compressed page");
    if (capacity > length) {
        uint64_t last = *offset + EGTB_BLOCK_HEADER_SIZE + capacity - 1;
        unsigned char zero = 0;
        if (!write_at(egtb->file, last, &zero, 1))
            return false;
    }
    return true;
}

static bool store_page(Egtb *egtb, uint64_t page, const EgtbEntry *entries,
                       bool exact)
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
    compressed_size = ZSTD_compressCCtx(egtb->compressor, egtb->compressed,
                                        egtb->compressed_capacity, entries,
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
                      egtb->compressed, length))
            return false;
        egtb->offsets[page] = old_offset;
    } else if (!append_block(egtb, checksum, egtb->compressed, length,
                             minimum, &egtb->offsets[page])) {
        return false;
    }
    egtb->lengths[page] = length;
    return true;
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
        entries[i].white_to_move = EGTB_DRAW;
        entries[i].black_to_move = EGTB_DRAW;
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
    size_t index = cache_find(cache, page);
    CacheEntry *entry;
    EgtbEntry *data;

    if (index != CACHE_NONE) {
        cache_touch(cache, index);
        *result = index;
        return true;
    }
    if (cache->used < cache->capacity) {
        index = cache->used++;
    } else {
        index = cache->lru_tail;
        if (!flush_cache_entry(egtb, index))
            return false;
        hash_remove(cache, index);
        lru_remove(cache, index);
    }
    entry = &cache->entries[index];
    data = cache_data(egtb, index);
    if (!load_page(egtb, page, data))
        return false;
    entry->page_index = page;
    entry->checksum = page_checksum(egtb, data);
    entry->dirty = false;
    entry->valid = true;
    entry->hash_next = CACHE_NONE;
    entry->lru_previous = CACHE_NONE;
    entry->lru_next = CACHE_NONE;
    hash_insert(cache, index);
    lru_push_front(cache, index);
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
    free(egtb);
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
    egtb->file = file;
    egtb->readonly = readonly;
    egtb->references = 1;
    egtb->path = malloc(strlen(path) + 1);
    if (egtb->path == NULL) {
        destroy_egtb(egtb);
        return fail("cannot allocate EGTB path");
    }
    strcpy(egtb->path, path);
    if (!read_header(egtb) || (!readonly && !load_directory(egtb)) ||
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
    for (current = readonly_registry; current != NULL;
         current = current->registry_next) {
        if (strcmp(current->path, path) == 0) {
            ++current->references;
            *out = current;
            return true;
        }
    }
    if (!open_common(out, path, true, cache_pages))
        return false;
    (*out)->registered = true;
    (*out)->registry_next = readonly_registry;
    readonly_registry = *out;
    return true;
}

bool egtb_open_readwrite(Egtb **out, const char *path, size_t cache_pages)
{
    Egtb *current;
    if (out == NULL || path == NULL)
        return fail("invalid read/write open argument");
    for (current = readonly_registry; current != NULL;
         current = current->registry_next) {
        if (strcmp(current->path, path) == 0)
            return fail("EGTB is already open read-only");
    }
    return open_common(out, path, false, cache_pages);
}

bool egtb_flush(Egtb *egtb)
{
    size_t i;
    if (egtb == NULL)
        return fail("cannot flush a null EGTB");
    if (egtb->readonly)
        return true;
    for (i = 0; i < egtb->cache.used; ++i) {
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
    if (egtb->registered && egtb->references > 1) {
        --egtb->references;
        return true;
    }
    if (egtb->registered) {
        Egtb **link = &readonly_registry;
        while (*link != egtb)
            link = &(*link)->registry_next;
        *link = egtb->registry_next;
    }
    if (!egtb->readonly)
        ok = egtb_flush(egtb);
    if (egtb->file != NULL && fclose(egtb->file) != 0)
        ok = fail("cannot close EGTB file");
    egtb->file = NULL;
    destroy_egtb(egtb);
    return ok;
}

static bool valid_value(int16_t value)
{
    return value == EGTB_DRAW || (value > 0 && value % 2 != 0) ||
           (value <= 0 && value % 2 == 0);
}

bool egtb_get(Egtb *egtb, uint64_t index, EgtbSide side, int16_t *value)
{
    uint64_t page;
    uint32_t slot;
    size_t cache_index;
    EgtbEntry *entries;
    if (egtb == NULL || value == NULL || index > egtb->maximum_index ||
        (side != EGTB_WHITE_TO_MOVE && side != EGTB_BLACK_TO_MOVE))
        return fail("invalid EGTB lookup");
    page = index / egtb->entries_per_page;
    slot = (uint32_t)(index % egtb->entries_per_page);
    if (!cached_page(egtb, page, &cache_index))
        return false;
    entries = cache_data(egtb, cache_index);
    *value = side == EGTB_WHITE_TO_MOVE ? entries[slot].white_to_move :
                                          entries[slot].black_to_move;
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
    if (egtb == NULL || egtb->readonly || index > egtb->maximum_index ||
        !valid_value(value) ||
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
        entries[slot].white_to_move = value;
    else
        entries[slot].black_to_move = value;
    cache_entry->checksum ^= slot_checksum(slot, &old) ^
                             slot_checksum(slot, &entries[slot]);
    cache_entry->dirty = true;
    return true;
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
