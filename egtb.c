#define _POSIX_C_SOURCE 200809L

#include "egtb.h"
#include "progress.h"

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

#if defined(__GNUC__) || defined(__clang__)
#define EGTB_LIKELY(condition) __builtin_expect(!!(condition), 1)
#define EGTB_COLD_NOINLINE __attribute__((cold, noinline))
#else
#define EGTB_LIKELY(condition) (condition)
#define EGTB_COLD_NOINLINE
#endif

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
    uint64_t side_page_count;
    uint64_t directory_offset;
    uint64_t data_offset;
    uint64_t *offsets;
    uint16_t *lengths;
    uint32_t page_size;
    uint32_t memory_page_size;
    uint32_t codec_capacity;
    uint32_t entries_per_page;
    uint32_t entry_index_mask;
    unsigned entry_index_shift;
    bool power_of_two_entries;
    unsigned reserve_percent;
    int compression_level;
    unsigned format_version;
    bool planar;
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
    unsigned char *codec;
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
    unsigned char *codec;
    size_t compressed_capacity;
    uint64_t first_page;
    uint64_t end_page;
    size_t slot_mask;
    bool dense_slots;
    bool power_of_two_slots;
    struct EgtbView *next;
    EgtbCacheStatistics statistics;
};

struct EgtbResident {
    Egtb *backing;
    EgtbEntry *entries;
    uint64_t allocated_bytes;
    uint64_t stored_histogram[2][65536];
};

static const unsigned char egtb_magic[8] = {'I','P','D','E','G','T','B','\0'};
static Egtb *readonly_registry;
static _Thread_local char last_error[256];
static pthread_mutex_t registry_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t crc32c_once = PTHREAD_ONCE_INIT;
static uint32_t crc32c_table[256];

_Static_assert(sizeof(EgtbEntry) == 4, "EgtbEntry must occupy four bytes");

static unsigned power_of_two_shift(size_t value)
{
    unsigned shift = 0;
    while (value > 1) {
        value >>= 1;
        ++shift;
    }
    return shift;
}

static void configure_entry_indexing(Egtb *egtb)
{
    uint32_t entries = egtb->entries_per_page;
    egtb->power_of_two_entries =
        entries != 0 && (entries & (entries - 1)) == 0;
    if (egtb->power_of_two_entries) {
        egtb->entry_index_mask = entries - 1;
        egtb->entry_index_shift = power_of_two_shift(entries);
    }
}

static inline void split_entry_index(const Egtb *egtb, uint64_t index,
                                     uint64_t *page, uint32_t *entry)
{
    if (EGTB_LIKELY(egtb->power_of_two_entries)) {
        *page = index >> egtb->entry_index_shift;
        *entry = (uint32_t)index & egtb->entry_index_mask;
    } else {
        *page = index / egtb->entries_per_page;
        *entry = (uint32_t)(index % egtb->entries_per_page);
    }
}

static inline void split_storage_index(const Egtb *egtb, uint64_t index,
                                       EgtbSide side, uint64_t *page,
                                       uint32_t *entry)
{
    split_entry_index(egtb, index, page, entry);
    if (egtb->planar)
        *page += (uint64_t)side * egtb->side_page_count;
}

static inline uint64_t position_page(const Egtb *egtb,
                                     uint64_t storage_page)
{
    return egtb->planar ? storage_page % egtb->side_page_count :
                          storage_page;
}

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

bool egtb_encode_dtm(int16_t value, int16_t *stored)
{
    if (stored == NULL)
        return false;
    if (value == EGTB_DRAW) {
        *stored = EGTB_STORED_DRAW;
        return true;
    }
    if (value > 0 && value <= EGTB_MAX_WIN_DTM && value % 2 != 0) {
        *stored = (int16_t)((value + 1) / 2);
        return true;
    }
    if (value <= 0 && value >= -EGTB_MAX_LOSS_DTM && value % 2 == 0) {
        *stored = (int16_t)(value / 2);
        return true;
    }
    return false;
}

int16_t egtb_decode_dtm(int16_t stored)
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
    header[8] = (unsigned char)egtb->format_version;
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

static bool configure_page_layout(Egtb *egtb)
{
    bool wide = egtb->format_version == EGTB_FORMAT_VERSION;
    if (egtb->format_version != EGTB_LEGACY_FORMAT_VERSION &&
        egtb->format_version != EGTB_BYTE_PLANAR_FORMAT_VERSION && !wide)
        return fail("unsupported EGTB version %u", egtb->format_version);
    egtb->planar = egtb->format_version >= EGTB_BYTE_PLANAR_FORMAT_VERSION;
    if (egtb->page_size == 0 || egtb->page_size > UINT16_MAX ||
        ((wide || !egtb->planar) && egtb->page_size % 2 != 0))
        return fail("invalid EGTB page size");
    egtb->memory_page_size = wide ? egtb->page_size : 2 * egtb->page_size;
    egtb->codec_capacity = wide ? 3 * (egtb->page_size / 2) : egtb->page_size;
    if (ZSTD_compressBound(egtb->codec_capacity) > UINT16_MAX)
        return fail("page codec is too large for 16-bit compressed lengths");
    egtb->entries_per_page = (wide || !egtb->planar)
                                ? egtb->page_size / 2 : egtb->page_size;
    configure_entry_indexing(egtb);
    return true;
}

static bool read_header(Egtb *egtb)
{
    unsigned char header[EGTB_HEADER_SIZE];
    uint64_t calculated_pages, entry_count;
    if (!read_at(egtb->file, 0, header, sizeof(header)))
        return false;
    if (memcmp(header, egtb_magic, sizeof(egtb_magic)) != 0)
        return fail("not an International Polish Draughts EGTB");
    if (get_u16(header + 10) != EGTB_HEADER_SIZE)
        return fail("unsupported EGTB header size");
    egtb->format_version = header[8];
    egtb->exact_layout = (header[9] & EGTB_FLAG_EXACT_LAYOUT) != 0;
    egtb->page_size = get_u32(header + 12);
    egtb->maximum_index = get_u64(header + 16);
    egtb->page_count = get_u64(header + 24);
    egtb->directory_offset = get_u64(header + 32);
    egtb->data_offset = get_u64(header + 40);
    egtb->reserve_percent = get_u16(header + 48);
    egtb->compression_level = (int16_t)get_u16(header + 50);
    if (!configure_page_layout(egtb))
        return false;
    if (egtb->maximum_index == UINT64_MAX)
        return fail("invalid EGTB maximum index");
    entry_count = egtb->maximum_index + 1;
    egtb->side_page_count = entry_count / egtb->entries_per_page +
                            (entry_count % egtb->entries_per_page != 0);
    if (egtb->planar && egtb->side_page_count > UINT64_MAX / 2)
        return fail("planar EGTB page count overflows");
    calculated_pages = egtb->planar ? 2 * egtb->side_page_count :
                                      egtb->side_page_count;
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

static void initialize_crc32c_table(void)
{
    unsigned entry;
    for (entry = 0; entry < 256; ++entry) {
        uint32_t value = entry;
        unsigned bit;
        for (bit = 0; bit < 8; ++bit)
            value = (value >> 1) ^
                    (UINT32_C(0x82f63b78) &
                     (UINT32_C(0) - (value & 1)));
        crc32c_table[entry] = value;
    }
}

static uint32_t crc32c(const void *data, size_t size)
{
    const unsigned char *bytes = data;
    uint32_t crc = UINT32_MAX;
    size_t i;
    pthread_once(&crc32c_once, initialize_crc32c_table);
    for (i = 0; i < size; ++i)
        crc = crc32c_table[(crc ^ bytes[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}


/* v4: 0x80 = draw, 0x7f = escape + little-endian signed 16-bit
 * half-distance. Other bytes are signed compact half-distances.
 * CRC32C covers the expanded little-endian 16-bit codes, not the stream.
 * v2/v3 retain their original byte payload and byte CRC. */
static int16_t page_code(const Egtb *egtb, const EgtbEntry *entries, size_t i)
{
    if (egtb->planar)
        return ((const int16_t *)(const void *)entries)[i];
    return i % 2 == 0 ? entries[i / 2].white_to_move
                       : entries[i / 2].black_to_move;
}

static void set_page_code(const Egtb *egtb, EgtbEntry *entries,
                          size_t i, int16_t code)
{
    if (egtb->planar)
        ((int16_t *)(void *)entries)[i] = code;
    else if (i % 2 == 0)
        entries[i / 2].white_to_move = code;
    else
        entries[i / 2].black_to_move = code;
}

static bool valid_code(const Egtb *egtb, int16_t code)
{
    int limit = egtb->format_version == EGTB_FORMAT_VERSION ? 16383 : 127;
    return code == EGTB_STORED_DRAW || (code >= -limit && code <= limit);
}

static bool encode_page(const Egtb *egtb, const EgtbEntry *entries,
                         unsigned char *codec, size_t *size, uint32_t *checksum)
{
    bool wide = egtb->format_version == EGTB_FORMAT_VERSION;
    unsigned char *canonical = codec + egtb->codec_capacity;
    size_t used = 0, count = egtb->memory_page_size / 2;
    for (size_t i = 0; i < count; ++i) {
        int16_t code = page_code(egtb, entries, i);
        if (!valid_code(egtb, code))
            return fail("DTM cannot be represented in this file version");
        if (wide)
            put_u16(canonical + 2 * i, (uint16_t)code);
        if (code == EGTB_STORED_DRAW)
            codec[used++] = 0x80;
        else if (!wide || (code >= -127 && code <= 126))
            codec[used++] = (unsigned char)code;
        else {
            codec[used++] = 0x7f;
            put_u16(codec + used, (uint16_t)code);
            used += 2;
        }
    }
    *size = used;
    *checksum = wide ? crc32c(canonical, egtb->memory_page_size)
                     : crc32c(codec, used);
    return true;
}

static bool decode_page(const Egtb *egtb, ZSTD_DCtx *decompressor,
                         const void *compressed, size_t length,
                         unsigned char *codec, EgtbEntry *entries,
                         uint32_t expected_checksum)
{
    bool wide = egtb->format_version == EGTB_FORMAT_VERSION;
    unsigned char *canonical = codec + egtb->codec_capacity;
    size_t size = ZSTD_decompressDCtx(decompressor, codec,
                                      egtb->codec_capacity, compressed, length);
    size_t used = 0, count = egtb->memory_page_size / 2;
    if (ZSTD_isError(size))
        return fail("Zstd decompression failed: %s", ZSTD_getErrorName(size));
    for (size_t i = 0; i < count; ++i) {
        int16_t code;
        unsigned byte;
        if (used == size)
            return fail("truncated DTM byte stream");
        byte = codec[used++];
        if (byte == 0x80)
            code = EGTB_STORED_DRAW;
        else if (wide && byte == 0x7f) {
            uint16_t raw;
            if (size - used < 2)
                return fail("truncated DTM escape");
            raw = get_u16(codec + used);
            code = (int16_t)(raw <= INT16_MAX ? (int)raw : (int)raw - 65536);
            used += 2;
            if (code == EGTB_STORED_DRAW || (code >= -127 && code <= 126))
                return fail("noncanonical DTM escape");
        } else
            code = (int16_t)(byte < 128 ? (int)byte : (int)byte - 256);
        if (!valid_code(egtb, code))
            return fail("invalid DTM code");
        set_page_code(egtb, entries, i, code);
        if (wide)
            put_u16(canonical + 2 * i, (uint16_t)code);
    }
    if (used != size)
        return fail("trailing bytes in DTM page");
    if ((wide ? crc32c(canonical, egtb->memory_page_size) : crc32c(codec, size))
        != expected_checksum)
        return fail("CRC32C mismatch for uncompressed page");
    return true;
}

static uint32_t slot_checksum(uint32_t slot, const EgtbEntry *entry)
{
    uint32_t values = (uint16_t)entry->white_to_move |
                      (uint32_t)(uint16_t)entry->black_to_move << 16;
    return mix32(values ^ mix32(slot + UINT32_C(0x9e3779b9)));
}

static uint32_t value_checksum(uint32_t slot, int16_t value)
{
    return mix32((uint16_t)value ^ mix32(slot + UINT32_C(0x9e3779b9)));
}

static uint32_t page_checksum(const Egtb *egtb, const EgtbEntry *entries)
{
    uint32_t checksum = 0;
    uint32_t i;
    if (egtb->planar) {
        const int16_t *values = (const int16_t *)(const void *)entries;
        for (i = 0; i < egtb->entries_per_page; ++i)
            checksum ^= value_checksum(i, values[i]);
        return checksum;
    }
    for (i = 0; i < egtb->entries_per_page; ++i)
        checksum ^= slot_checksum(i, &entries[i]);
    return checksum;
}

static size_t cache_slot_for_page(const Egtb *egtb, uint64_t page,
                                  size_t capacity)
{
    if (!egtb->planar)
        return (size_t)(page % capacity);
    {
        size_t per_side = capacity / 2;
        size_t side = (size_t)(page / egtb->side_page_count);
        return side * per_side +
               (size_t)(page % egtb->side_page_count) % per_side;
    }
}

static bool cache_init(Egtb *egtb, size_t capacity)
{
    PageCache *cache = &egtb->cache;
    if (capacity == 0)
        return fail("page cache must contain at least one page");
    if (egtb->planar && capacity < 2)
        capacity = 2;
    if (egtb->page_count < capacity)
        capacity = (size_t)egtb->page_count;
    if (egtb->planar && (capacity & 1) != 0)
        --capacity;
    if (capacity == 0)
        capacity = 1;
    if (capacity > SIZE_MAX / egtb->memory_page_size)
        return fail("page cache byte size overflows size_t");
    cache->entries = calloc(capacity, sizeof(*cache->entries));
    cache->data = malloc(capacity * egtb->memory_page_size);
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
    return (EgtbEntry *)(void *)(egtb->cache.data + index * egtb->memory_page_size);
}

static int16_t stored_value(const Egtb *egtb, const EgtbEntry *entries,
                           uint32_t slot, EgtbSide side)
{
    if (egtb->planar)
        return ((const int16_t *)(const void *)entries)[slot];
    return side == EGTB_WHITE_TO_MOVE ? entries[slot].white_to_move :
                                        entries[slot].black_to_move;
}

static void replace_stored_value(const Egtb *egtb, EgtbEntry *entries,
                                 uint32_t slot, EgtbSide side, int16_t value)
{
    if (egtb->planar)
        ((int16_t *)(void *)entries)[slot] = value;
    else if (side == EGTB_WHITE_TO_MOVE)
        entries[slot].white_to_move = value;
    else
        entries[slot].black_to_move = value;
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
    if (egtb->planar) {
        const int16_t *values = (const int16_t *)(const void *)entries;
        for (i = 0; i < egtb->entries_per_page; ++i)
            if (values[i] != EGTB_STORED_DRAW)
                return false;
        return true;
    }
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
                                    unsigned char *codec, uint64_t page,
                                    const EgtbEntry *entries, bool exact,
                                    bool synchronize_append)
{
    uint64_t old_offset = egtb->offsets[page];
    uint16_t old_length = egtb->lengths[page];
    size_t compressed_size, encoded_size;
    uint16_t length, minimum, available;
    uint32_t checksum;
    unsigned char checksum_bytes[EGTB_BLOCK_HEADER_SIZE];

    if (all_draws(egtb, entries)) {
        egtb->offsets[page] = 0;
        egtb->lengths[page] = 0;
        return true;
    }
    if (!encode_page(egtb, entries, codec, &encoded_size, &checksum))
        return false;
    compressed_size = ZSTD_compressCCtx(compressor, compressed,
                                        compressed_capacity, codec,
                                        encoded_size,
                                        egtb->compression_level);
    if (ZSTD_isError(compressed_size))
        return fail("Zstd compression failed: %s",
                    ZSTD_getErrorName(compressed_size));
    if (compressed_size == 0 || compressed_size > UINT16_MAX)
        return fail("compressed page does not fit in 16-bit length");
    length = (uint16_t)compressed_size;
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
        egtb->compressed_capacity, egtb->codec, page, entries, exact, false);
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
    if (egtb->planar) {
        int16_t *values = (int16_t *)(void *)entries;
        for (i = 0; i < egtb->entries_per_page; ++i)
            values[i] = EGTB_STORED_DRAW;
        return;
    }
    for (i = 0; i < egtb->entries_per_page; ++i) {
        entries[i].white_to_move = EGTB_STORED_DRAW;
        entries[i].black_to_move = EGTB_STORED_DRAW;
    }
}

static bool load_page(Egtb *egtb, uint64_t page, EgtbEntry *entries)
{
    uint64_t offset;
    uint16_t length;
    unsigned char checksum_bytes[EGTB_BLOCK_HEADER_SIZE];
    uint32_t expected_checksum;
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
    return decode_page(egtb, egtb->decompressor, egtb->compressed, length,
                       egtb->codec, entries, expected_checksum);
}

static bool cached_page(Egtb *egtb, uint64_t page, size_t *result)
{
    PageCache *cache = &egtb->cache;
    size_t index = cache_slot_for_page(egtb, page, cache->capacity);
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
    free(egtb->codec);
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
    egtb->compressed_capacity = ZSTD_compressBound(egtb->codec_capacity);
    if (egtb->compressed_capacity > UINT16_MAX)
        return fail("page size is too large for 16-bit compressed lengths");
    egtb->compressed = malloc(egtb->compressed_capacity);
    egtb->codec = malloc(egtb->codec_capacity + egtb->memory_page_size);
    egtb->compressor = ZSTD_createCCtx();
    egtb->decompressor = ZSTD_createDCtx();
    if (egtb->codec == NULL || egtb->compressed == NULL || egtb->compressor == NULL ||
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

static bool create_with_version(Egtb **out, const char *path,
                                uint64_t maximum_index, uint32_t page_size,
                                const EgtbCreateOptions *options,
                                unsigned format_version)
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
        options->cache_pages == 0 ||
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
    egtb->format_version = format_version;
    egtb->page_size = page_size;
    if (!configure_page_layout(egtb)) {
        destroy_egtb(egtb);
        return false;
    }
    egtb->maximum_index = maximum_index;
    entries = maximum_index + 1;
    egtb->side_page_count = entries / egtb->entries_per_page +
                            (entries % egtb->entries_per_page != 0);
    if (egtb->planar && egtb->side_page_count > UINT64_MAX / 2) {
        destroy_egtb(egtb);
        return fail("planar EGTB page count overflows");
    }
    egtb->page_count = egtb->planar ? 2 * egtb->side_page_count :
                                      egtb->side_page_count;
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

bool egtb_create(Egtb **out, const char *path, uint64_t maximum_index,
                 uint32_t page_size, const EgtbCreateOptions *options)
{
    return create_with_version(out, path, maximum_index, page_size, options,
                               EGTB_FORMAT_VERSION);
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
    split_storage_index(egtb, index, side, &page, &slot);
    if (!cached_page(egtb, page, &cache_index))
        return false;
    entries = cache_data(egtb, cache_index);
    *value = egtb_decode_dtm(stored_value(egtb, entries, slot, side));
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
    int16_t old_value;
    int16_t stored;
    if (egtb == NULL || egtb->readonly || egtb->writable_views != 0 ||
        index > egtb->maximum_index ||
        !egtb_encode_dtm(value, &stored) ||
        !valid_code(egtb, stored) ||
        (side != EGTB_WHITE_TO_MOVE && side != EGTB_BLACK_TO_MOVE))
        return fail("invalid EGTB update");
    split_storage_index(egtb, index, side, &page, &slot);
    if (!cached_page(egtb, page, &cache_index))
        return false;
    cache_entry = &egtb->cache.entries[cache_index];
    entries = cache_data(egtb, cache_index);
    if (egtb->planar) {
        old_value = stored_value(egtb, entries, slot, side);
        replace_stored_value(egtb, entries, slot, side, stored);
        cache_entry->checksum ^= value_checksum(slot, old_value) ^
                                 value_checksum(slot, stored);
    } else {
        old = entries[slot];
        replace_stored_value(egtb, entries, slot, side, stored);
        cache_entry->checksum ^= slot_checksum(slot, &old) ^
                                 slot_checksum(slot, &entries[slot]);
    }
    cache_entry->dirty = true;
    return true;
}

bool egtb_set_pair(Egtb *egtb, uint64_t index,
                   int16_t white_to_move, int16_t black_to_move)
{
    uint64_t page;
    uint32_t slot;
    size_t cache_index;
    CacheEntry *cache_entry;
    EgtbEntry *entries;
    EgtbEntry old, replacement;
    if (egtb == NULL || egtb->readonly || egtb->writable_views != 0 ||
        index > egtb->maximum_index ||
        !egtb_encode_dtm(white_to_move, &replacement.white_to_move) ||
        !egtb_encode_dtm(black_to_move, &replacement.black_to_move) ||
        !valid_code(egtb, replacement.white_to_move) ||
        !valid_code(egtb, replacement.black_to_move))
        return fail("invalid paired EGTB update");
    if (egtb->planar)
        return egtb_set(egtb, index, EGTB_WHITE_TO_MOVE, white_to_move) &&
               egtb_set(egtb, index, EGTB_BLACK_TO_MOVE, black_to_move);
    split_entry_index(egtb, index, &page, &slot);
    if (!cached_page(egtb, page, &cache_index))
        return false;
    cache_entry = &egtb->cache.entries[cache_index];
    entries = cache_data(egtb, cache_index);
    old = entries[slot];
    entries[slot] = replacement;
    cache_entry->checksum ^= slot_checksum(slot, &old) ^
                             slot_checksum(slot, &replacement);
    cache_entry->dirty = true;
    return true;
}

static EgtbEntry *view_cache_data(EgtbView *view, size_t slot)
{
    return (EgtbEntry *)(void *)(view->data +
                                 slot * view->backing->memory_page_size);
}

static bool view_load_page(EgtbView *view, uint64_t page, EgtbEntry *entries)
{
    Egtb *egtb = view->backing;
    uint64_t offset = egtb->offsets[page];
    uint16_t length = egtb->lengths[page];
    unsigned char checksum_bytes[EGTB_BLOCK_HEADER_SIZE];
    uint32_t expected_checksum;
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
    if (!decode_page(egtb, view->decompressor, view->compressed, length,
                      view->codec, entries, expected_checksum))
        return false;
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
            view->compressed_capacity, view->codec, entry->page_index, data, false,
            true)) {
        return false;
    }
    if (compressed_write)
        ++view->statistics.compressed_writes;
    entry->dirty = false;
    return true;
}

static inline size_t view_cache_slot(const EgtbView *view, uint64_t page)
{
    if (view->backing->planar) {
        uint64_t logical = page % view->backing->side_page_count;
        size_t per_side = view->capacity / 2;
        size_t side = (size_t)(page / view->backing->side_page_count);
        if (EGTB_LIKELY(view->dense_slots))
            return side * per_side + (size_t)(logical - view->first_page);
        return side * per_side + (size_t)logical % per_side;
    }
    if (EGTB_LIKELY(view->dense_slots))
        return (size_t)(page - view->first_page);
    if (EGTB_LIKELY(view->power_of_two_slots))
        return (size_t)page & view->slot_mask;
    return (size_t)(page % view->capacity);
}

#ifndef NDEBUG
static bool view_contains_page(const EgtbView *view, uint64_t page)
{
    uint64_t logical = position_page(view->backing, page);
    return logical >= view->first_page && logical < view->end_page;
}
#endif

static EgtbEntry *EGTB_COLD_NOINLINE
view_cache_miss(EgtbView *view, uint64_t page, size_t slot, EgtbEntry *data)
{
    DirectCacheEntry *entry = &view->entries[slot];
    ++view->statistics.misses;
    if (entry->valid && entry->dirty) {
        ++view->statistics.dirty_evictions;
        if (!view_flush_slot(view, slot))
            return NULL;
    }
    if (!view_load_page(view, page, data))
        return NULL;
    entry->page_index = page;
    entry->checksum = page_checksum(view->backing, data);
    entry->valid = true;
    entry->dirty = false;
    return data;
}

static inline EgtbEntry *view_cached_page(EgtbView *view, uint64_t page,
                                          size_t *result_slot)
{
    size_t slot = view_cache_slot(view, page);
    DirectCacheEntry *entry = &view->entries[slot];
    EgtbEntry *data = view_cache_data(view, slot);
    ++view->statistics.lookups;
    if (result_slot != NULL)
        *result_slot = slot;
    if (EGTB_LIKELY(entry->valid && entry->page_index == page)) {
        ++view->statistics.hits;
        return data;
    }
    return view_cache_miss(view, page, slot, data);
}

bool egtb_view_create(EgtbView **out, Egtb *backing, size_t cache_pages,
                      bool writable)
{
    return egtb_view_create_range(out, backing, cache_pages, writable, 0,
                                  backing == NULL ? 0 :
                                      backing->side_page_count);
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
        first_page >= end_page || end_page > backing->side_page_count)
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
    if (backing->planar) {
        uint64_t physical_range = 2 * range_pages;
        if (physical_range < cache_pages)
            cache_pages = (size_t)physical_range;
        if (cache_pages < 2)
            cache_pages = 2;
        if ((cache_pages & 1) != 0)
            --cache_pages;
    } else if (range_pages < cache_pages) {
        cache_pages = (size_t)range_pages;
    }
    if (cache_pages == 0)
        cache_pages = 1;
    if (cache_pages > SIZE_MAX / backing->memory_page_size)
        return fail("cache-view byte size overflows size_t");
    view = calloc(1, sizeof(*view));
    if (view == NULL)
        return fail("cannot allocate cache view");
    view->backing = backing;
    view->capacity = cache_pages;
    view->writable = writable;
    view->first_page = first_page;
    view->end_page = end_page;
    view->dense_slots = cache_pages ==
        (size_t)(backing->planar ? 2 * range_pages : range_pages);
    view->power_of_two_slots =
        !backing->planar && !view->dense_slots &&
        (cache_pages & (cache_pages - 1)) == 0;
    if (view->power_of_two_slots)
        view->slot_mask = cache_pages - 1;
    view->compressed_capacity = ZSTD_compressBound(backing->codec_capacity);
    view->entries = calloc(cache_pages, sizeof(*view->entries));
    view->data = malloc(cache_pages * backing->memory_page_size);
    view->compressed = malloc(view->compressed_capacity);
    view->codec = malloc(backing->codec_capacity + backing->memory_page_size);
    view->decompressor = ZSTD_createDCtx();
    if (writable)
        view->compressor = ZSTD_createCCtx();
    if (view->entries == NULL || view->data == NULL ||
        view->codec == NULL || view->compressed == NULL || view->decompressor == NULL ||
        (writable && view->compressor == NULL)) {
        ZSTD_freeCCtx(view->compressor);
        ZSTD_freeDCtx(view->decompressor);
        free(view->codec);
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
    free(view->codec);
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
    EgtbEntry *entries;
#ifndef NDEBUG
    if (view == NULL || value == NULL)
        return fail("invalid cache-view lookup");
#endif
    egtb = view->backing;
#ifndef NDEBUG
    if (index > egtb->maximum_index ||
        (side != EGTB_WHITE_TO_MOVE && side != EGTB_BLACK_TO_MOVE))
        return fail("invalid cache-view lookup");
#endif
    split_storage_index(egtb, index, side, &page, &entry_index);
#ifndef NDEBUG
    if (!view_contains_page(view, page))
        return fail("cache-view lookup is outside its page range");
#endif
    entries = view_cached_page(view, page, NULL);
    if (entries == NULL)
        return false;
    *value = egtb_decode_dtm(
        stored_value(egtb, entries, entry_index, side));
    return true;
}

bool egtb_view_get_pair(EgtbView *view, uint64_t index,
                        int16_t *white_to_move, int16_t *black_to_move)
{
    Egtb *egtb;
    uint64_t page;
    uint32_t entry_index;
    EgtbEntry *entries;
#ifndef NDEBUG
    if (view == NULL || white_to_move == NULL || black_to_move == NULL)
        return fail("invalid paired cache-view lookup");
#endif
    egtb = view->backing;
#ifndef NDEBUG
    if (index > egtb->maximum_index)
        return fail("invalid paired cache-view lookup");
#endif
    if (egtb->planar)
        return egtb_view_get(view, index, EGTB_WHITE_TO_MOVE,
                             white_to_move) &&
               egtb_view_get(view, index, EGTB_BLACK_TO_MOVE,
                             black_to_move);
    split_entry_index(egtb, index, &page, &entry_index);
#ifndef NDEBUG
    if (page < view->first_page || page >= view->end_page)
        return fail("paired cache-view lookup is outside its page range");
#endif
    entries = view_cached_page(view, page, NULL);
    if (entries == NULL)
        return false;
    *white_to_move = egtb_decode_dtm(entries[entry_index].white_to_move);
    *black_to_move = egtb_decode_dtm(entries[entry_index].black_to_move);
    return true;
}

bool egtb_sequential_reader_init(EgtbSequentialReader *reader,
                                 EgtbView *view, uint64_t first_index,
                                 uint64_t end_index)
{
    Egtb *egtb;
    uint64_t first_page, last_page;
    uint32_t ignored;
    if (reader == NULL || view == NULL)
        return fail("invalid sequential-reader request");
    egtb = view->backing;
    if (first_index > end_index || end_index > egtb->maximum_index + 1)
        return fail("invalid sequential-reader range");
    if (first_index != end_index) {
        split_entry_index(egtb, first_index, &first_page, &ignored);
        split_entry_index(egtb, end_index - 1, &last_page, &ignored);
        if (first_page < view->first_page || last_page >= view->end_page)
            return fail("sequential-reader range is outside its cache view");
    }
    reader->view = view;
    reader->next_index = first_index;
    reader->end_index = end_index;
    reader->next_entry = NULL;
    reader->page_end = NULL;
    reader->next_white = NULL;
    reader->next_black = NULL;
    reader->plane_end = NULL;
    return true;
}

bool egtb_sequential_reader_next(EgtbSequentialReader *reader,
                                 int16_t *white_to_move,
                                 int16_t *black_to_move)
{
    const EgtbEntry *entry;
#ifndef NDEBUG
    if (reader == NULL || reader->view == NULL || white_to_move == NULL ||
        black_to_move == NULL || reader->next_index >= reader->end_index)
        return fail("invalid or exhausted sequential-reader lookup");
#endif
    if (reader->view->backing->planar) {
        if (reader->next_white == reader->plane_end) {
            Egtb *egtb = reader->view->backing;
            uint64_t logical_page;
            uint32_t entry_index;
            EgtbEntry *white_page, *black_page;
            split_entry_index(egtb, reader->next_index, &logical_page,
                              &entry_index);
            white_page = view_cached_page(reader->view, logical_page, NULL);
            black_page = view_cached_page(
                reader->view, logical_page + egtb->side_page_count, NULL);
            if (white_page == NULL || black_page == NULL)
                return false;
            reader->next_white =
                (const int16_t *)(const void *)white_page + entry_index;
            reader->next_black =
                (const int16_t *)(const void *)black_page + entry_index;
            reader->plane_end =
                (const int16_t *)(const void *)white_page +
                egtb->entries_per_page;
        }
        *white_to_move = egtb_decode_dtm(*reader->next_white++);
        *black_to_move = egtb_decode_dtm(*reader->next_black++);
        ++reader->next_index;
        return true;
    }
    if (reader->next_entry == reader->page_end) {
        Egtb *egtb = reader->view->backing;
        EgtbEntry *entries;
        uint64_t page;
        uint64_t remaining;
        uint32_t entry_index;
        uint32_t available;
        split_entry_index(egtb, reader->next_index, &page, &entry_index);
        entries = view_cached_page(reader->view, page, NULL);
        if (entries == NULL)
            return false;
        available = egtb->entries_per_page - entry_index;
        remaining = reader->end_index - reader->next_index;
        if (remaining < available)
            available = (uint32_t)remaining;
        reader->next_entry = entries + entry_index;
        reader->page_end = reader->next_entry + available;
    }
    entry = reader->next_entry++;
    ++reader->next_index;
    *white_to_move = egtb_decode_dtm(entry->white_to_move);
    *black_to_move = egtb_decode_dtm(entry->black_to_move);
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
    int16_t old_value;
    int16_t stored;
#ifndef NDEBUG
    if (view == NULL || !view->writable)
        return fail("invalid cache-view update");
#endif
    if (!egtb_encode_dtm(value, &stored) || !valid_code(view->backing, stored))
        return fail("invalid cache-view update");
    egtb = view->backing;
#ifndef NDEBUG
    if (index > egtb->maximum_index ||
        (side != EGTB_WHITE_TO_MOVE && side != EGTB_BLACK_TO_MOVE))
        return fail("invalid cache-view update");
#endif
    split_storage_index(egtb, index, side, &page, &entry_index);
#ifndef NDEBUG
    if (!view_contains_page(view, page))
        return fail("cache-view update is outside its page range");
#endif
    entries = view_cached_page(view, page, &cache_slot);
    if (entries == NULL)
        return false;
    cache_entry = &view->entries[cache_slot];
    if (egtb->planar) {
        old_value = stored_value(egtb, entries, entry_index, side);
        replace_stored_value(egtb, entries, entry_index, side, stored);
        cache_entry->checksum ^= value_checksum(entry_index, old_value) ^
                                 value_checksum(entry_index, stored);
    } else {
        old = entries[entry_index];
        replace_stored_value(egtb, entries, entry_index, side, stored);
        cache_entry->checksum ^= slot_checksum(entry_index, &old) ^
                                 slot_checksum(entry_index,
                                               &entries[entry_index]);
    }
    cache_entry->dirty = true;
    return true;
}

bool egtb_view_write_page(EgtbView *view, uint64_t page,
                          const EgtbEntry *entries, size_t count)
{
    Egtb *egtb;
    uint64_t remaining;
    size_t expected;
    if (view == NULL || !view->writable || entries == NULL ||
        page < view->first_page || page >= view->end_page)
        return fail("invalid complete-page update");
    egtb = view->backing;
    remaining = egtb->maximum_index + 1 - page * egtb->entries_per_page;
    expected = remaining < egtb->entries_per_page
                   ? (size_t)remaining : egtb->entries_per_page;
    if (count != expected)
        return fail("complete-page update has incorrect entry count");
    for (unsigned side = 0; side < (egtb->planar ? 2u : 1u); ++side) {
        uint64_t physical = page + side * egtb->side_page_count;
        size_t slot = view_cache_slot(view, physical);
        DirectCacheEntry *entry = &view->entries[slot];
        EgtbEntry *data = view_cache_data(view, slot);
        if (!view_flush_slot(view, slot))
            return false;
        fill_draw_page(egtb, data);
        if (egtb->planar) {
            int16_t *plane = (int16_t *)data;
            for (size_t i = 0; i < count; ++i)
                plane[i] = side == 0 ? entries[i].white_to_move
                                     : entries[i].black_to_move;
        } else {
            memcpy(data, entries, count * sizeof(*entries));
        }
        entry->page_index = physical;
        entry->checksum = page_checksum(egtb, data);
        entry->valid = true;
        entry->dirty = true;
        if (!view_flush_slot(view, slot))
            return false;
    }
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

typedef struct {
    Egtb *backing;
    EgtbEntry *entries;
    uint64_t first_page;
    uint64_t end_page;
    uint64_t stored_histogram[2][65536];
    bool failed;
    char error[256];
} ResidentLoadWorker;

static void *load_resident_pages(void *opaque)
{
    ResidentLoadWorker *worker = opaque;
    Egtb *egtb = worker->backing;
    ZSTD_DCtx *decompressor = ZSTD_createDCtx();
    size_t compressed_capacity = ZSTD_compressBound(egtb->codec_capacity);
    unsigned char *compressed = malloc(compressed_capacity);
    unsigned char *decoded = malloc(egtb->memory_page_size);
    unsigned char *codec = malloc(egtb->codec_capacity + egtb->memory_page_size);
    uint64_t page;
    if (decompressor == NULL || compressed == NULL || decoded == NULL || codec == NULL) {
        snprintf(worker->error, sizeof(worker->error),
                 "cannot allocate resident decompression workspace");
        worker->failed = true;
        goto done;
    }
    for (page = worker->first_page; page < worker->end_page; ++page) {
        uint64_t logical_page = position_page(egtb, page);
        EgtbSide planar_side = page < egtb->side_page_count
                                   ? EGTB_WHITE_TO_MOVE
                                   : EGTB_BLACK_TO_MOVE;
        EgtbEntry *destination = (EgtbEntry *)(void *)decoded;
        uint64_t offset = egtb->offsets[page];
        uint16_t length = egtb->lengths[page];
        uint64_t first_index = logical_page * egtb->entries_per_page;
        uint64_t remaining = egtb->maximum_index - first_index + 1;
        uint32_t valid_entries = remaining < egtb->entries_per_page
                                     ? (uint32_t)remaining
                                     : egtb->entries_per_page;
        unsigned char checksum_bytes[EGTB_BLOCK_HEADER_SIZE];
        uint32_t expected_checksum;
            if (offset == 0) {
            fill_draw_page(egtb, destination);
            if (egtb->planar) {
                worker->stored_histogram[planar_side]
                                        [(uint16_t)EGTB_STORED_DRAW] +=
                    valid_entries;
                for (uint32_t entry = 0; entry < valid_entries; ++entry)
                    if (planar_side == EGTB_WHITE_TO_MOVE)
                        worker->entries[first_index + entry].white_to_move =
                            EGTB_STORED_DRAW;
                    else
                        worker->entries[first_index + entry].black_to_move =
                            EGTB_STORED_DRAW;
            } else {
                memcpy(&worker->entries[first_index], destination,
                       valid_entries * sizeof(EgtbEntry));
                worker->stored_histogram[EGTB_WHITE_TO_MOVE]
                                        [(uint16_t)EGTB_STORED_DRAW] +=
                    valid_entries;
                worker->stored_histogram[EGTB_BLACK_TO_MOVE]
                                        [(uint16_t)EGTB_STORED_DRAW] +=
                    valid_entries;
            }
            egtb_progress_add(1);
            continue;
        }
        if (length > compressed_capacity ||
            !pread_at(fileno(egtb->file), offset, checksum_bytes,
                      sizeof(checksum_bytes)) ||
            !pread_at(fileno(egtb->file),
                      offset + EGTB_BLOCK_HEADER_SIZE,
                      compressed, length)) {
            snprintf(worker->error, sizeof(worker->error), "%s",
                     egtb_last_error());
            worker->failed = true;
            break;
        }
        expected_checksum = get_u32(checksum_bytes);
        if (!decode_page(egtb, decompressor, compressed, length, codec,
                          destination, expected_checksum)) {
            snprintf(worker->error, sizeof(worker->error),
                     "page %" PRIu64 ": %s", page, egtb_last_error());
            worker->failed = true;
            break;
        }
        for (uint32_t entry = 0; entry < valid_entries; ++entry) {
            if (egtb->planar) {
                int16_t value = ((int16_t *)(void *)destination)[entry];
                if (planar_side == EGTB_WHITE_TO_MOVE)
                    worker->entries[first_index + entry].white_to_move = value;
                else
                    worker->entries[first_index + entry].black_to_move = value;
                ++worker->stored_histogram[planar_side][(uint16_t)value];
            } else {
                ++worker->stored_histogram[EGTB_WHITE_TO_MOVE]
                                          [(uint16_t)destination[entry]
                                               .white_to_move];
                ++worker->stored_histogram[EGTB_BLACK_TO_MOVE]
                                          [(uint16_t)destination[entry]
                                               .black_to_move];
            }
        }
        if (!egtb->planar)
            memcpy(&worker->entries[first_index], destination,
                   valid_entries * sizeof(EgtbEntry));
        egtb_progress_add(1);
    }
done:
    free(codec);
    free(decoded);
    free(compressed);
    ZSTD_freeDCtx(decompressor);
    return NULL;
}

bool egtb_resident_load(EgtbResident **out, Egtb *backing,
                        unsigned thread_count)
{
    EgtbResident *resident = NULL;
    ResidentLoadWorker *workers = NULL;
    pthread_t *threads = NULL;
    uint64_t bytes, pages_per_worker, extra_pages;
    unsigned i, created_threads = 0;
    bool ok = false;
    if (out == NULL)
        return fail("invalid resident EGTB output pointer");
    *out = NULL;
    if (backing == NULL || !backing->readonly || thread_count == 0)
        return fail("resident EGTB requires a read-only backing and workers");
    if (backing->maximum_index + 1 > UINT64_MAX / sizeof(EgtbEntry))
        return fail("resident EGTB size overflows uint64_t");
    bytes = (backing->maximum_index + 1) * sizeof(EgtbEntry);
    if (bytes > SIZE_MAX)
        return fail("resident EGTB does not fit in address space");
    if (backing->page_count < thread_count)
        thread_count = (unsigned)backing->page_count;
    egtb_progress_begin("resident load", backing->page_count, "pages");
    resident = calloc(1, sizeof(*resident));
    workers = calloc(thread_count, sizeof(*workers));
    threads = calloc(thread_count, sizeof(*threads));
    if (resident == NULL || workers == NULL || threads == NULL ||
        (resident->entries = malloc((size_t)bytes)) == NULL) {
        fail("cannot allocate resident EGTB");
        goto done;
    }
    resident->backing = backing;
    resident->allocated_bytes = bytes;
    pages_per_worker = backing->page_count / thread_count;
    extra_pages = backing->page_count % thread_count;
    for (i = 0; i < thread_count; ++i) {
        uint64_t first_page = i * pages_per_worker +
                              (i < extra_pages ? i : extra_pages);
        workers[i].backing = backing;
        workers[i].entries = resident->entries;
        workers[i].first_page = first_page;
        workers[i].end_page = first_page + pages_per_worker +
                              (i < extra_pages);
        {
            int error = pthread_create(&threads[i], NULL,
                                       load_resident_pages, &workers[i]);
            if (error != 0) {
                fail("cannot create resident loader %u: %s", i,
                     strerror(error));
                break;
            }
        }
        ++created_threads;
    }
    for (i = 0; i < created_threads; ++i) {
        int error = pthread_join(threads[i], NULL);
        if (error != 0) {
            fail("cannot join resident loader %u: %s", i, strerror(error));
            goto done;
        }
    }
    if (created_threads != thread_count)
        goto done;
    for (i = 0; i < thread_count; ++i)
        if (workers[i].failed) {
            fail("resident loader %u failed: %s", i, workers[i].error);
            goto done;
        }
    for (i = 0; i < thread_count; ++i)
        for (unsigned side = 0; side < 2; ++side)
            for (unsigned stored = 0; stored < 65536; ++stored)
                resident->stored_histogram[side][stored] +=
                    workers[i].stored_histogram[side][stored];
    *out = resident;
    resident = NULL;
    ok = true;
done:
    egtb_progress_end(ok);
    if (resident != NULL) {
        free(resident->entries);
        free(resident);
    }
    free(threads);
    free(workers);
    return ok;
}

void egtb_resident_destroy(EgtbResident *resident)
{
    if (resident == NULL)
        return;
    free(resident->entries);
    free(resident);
}

bool egtb_resident_get(const EgtbResident *resident, uint64_t index,
                       EgtbSide side, int16_t *value)
{
#ifndef NDEBUG
    if (resident == NULL || value == NULL ||
        index > resident->backing->maximum_index ||
        (side != EGTB_WHITE_TO_MOVE && side != EGTB_BLACK_TO_MOVE))
        return fail("invalid resident EGTB lookup");
#endif
    *value = egtb_decode_dtm(side == EGTB_WHITE_TO_MOVE
                                ? resident->entries[index].white_to_move
                                : resident->entries[index].black_to_move);
    return true;
}

bool egtb_resident_get_pair(const EgtbResident *resident, uint64_t index,
                            int16_t *white_to_move,
                            int16_t *black_to_move)
{
    const EgtbEntry *entry;
#ifndef NDEBUG
    if (resident == NULL || white_to_move == NULL || black_to_move == NULL ||
        index > resident->backing->maximum_index)
        return fail("invalid paired resident EGTB lookup");
#endif
    entry = &resident->entries[index];
    *white_to_move = egtb_decode_dtm(entry->white_to_move);
    *black_to_move = egtb_decode_dtm(entry->black_to_move);
    return true;
}

bool egtb_resident_matches(const EgtbResident *resident,
                           const Egtb *backing)
{
    return resident != NULL && resident->backing == backing;
}

uint64_t egtb_resident_bytes(const EgtbResident *resident)
{
    return resident == NULL ? 0 : resident->allocated_bytes;
}

bool egtb_resident_dtm_histogram(const EgtbResident *resident,
                                 uint64_t *histogram,
                                 size_t bins_per_side)
{
    if (resident == NULL || histogram == NULL ||
        bins_per_side < UINT16_MAX + 1u)
        return fail("invalid resident DTM histogram output");
    memset(histogram, 0, 2 * bins_per_side * sizeof(*histogram));
    for (unsigned side = 0; side < 2; ++side)
        for (int stored = -16383; stored <= 16383; ++stored) {
            uint16_t bucket = (uint16_t)stored;
            int16_t dtm = egtb_decode_dtm((int16_t)stored);
            histogram[(size_t)side * bins_per_side + (uint16_t)dtm] +=
                resident->stored_histogram[side][bucket];
        }
    for (unsigned side = 0; side < 2; ++side)
        histogram[(size_t)side * bins_per_side + (uint16_t)EGTB_DRAW] +=
            resident->stored_histogram[side][(uint16_t)EGTB_STORED_DRAW];
    return true;
}

static void consider_dtm_example(EgtbDtmExamples *examples, uint64_t index,
                                 EgtbSide side, int16_t value)
{
    EgtbDtmExample *example;
    if (value == EGTB_DRAW) {
        if (!examples->draw.available ||
            side < examples->draw_side ||
            (side == examples->draw_side && index < examples->draw.index)) {
            examples->draw.index = index;
            examples->draw.dtm = value;
            examples->draw.available = true;
            examples->draw_side = side;
        }
        return;
    }
    example = value > 0 ? &examples->longest_win[side]
                        : &examples->longest_loss[side];
    if (!example->available ||
        (value > 0 ? value > example->dtm : value < example->dtm) ||
        (value == example->dtm && index < example->index)) {
        example->index = index;
        example->dtm = value;
        example->available = true;
    }
}

bool egtb_find_dtm_examples(Egtb *backing, const EgtbResident *resident,
                            EgtbDtmExamples *examples)
{
    uint64_t positions, pending = 0;
    if (backing == NULL || examples == NULL ||
        (resident != NULL && !egtb_resident_matches(resident, backing)))
        return fail("invalid DTM examples scan");
    memset(examples, 0, sizeof(*examples));
    examples->draw_side = EGTB_BLACK_TO_MOVE;
    positions = backing->maximum_index + 1;
    if (resident != NULL) {
        for (uint64_t index = 0; index < positions; ++index) {
            int16_t white, black;
            if (!egtb_resident_get_pair(resident, index, &white, &black))
                return false;
            consider_dtm_example(examples, index, EGTB_WHITE_TO_MOVE, white);
            consider_dtm_example(examples, index, EGTB_BLACK_TO_MOVE, black);
            egtb_progress_tick(&pending);
        }
        egtb_progress_flush(&pending);
        return true;
    }
    {
        EgtbView *view = NULL;
        EgtbSequentialReader reader;
        bool ok = false;
        if (!egtb_view_create(&view, backing, 1, false) ||
            !egtb_sequential_reader_init(&reader, view, 0, positions))
            goto done;
        for (uint64_t index = 0; index < positions; ++index) {
            int16_t white, black;
            if (!egtb_sequential_reader_next(&reader, &white, &black))
                goto done;
            consider_dtm_example(examples, index, EGTB_WHITE_TO_MOVE, white);
            consider_dtm_example(examples, index, EGTB_BLACK_TO_MOVE, black);
            egtb_progress_tick(&pending);
        }
        egtb_progress_flush(&pending);
        ok = true;
done:
        if (view != NULL && !egtb_view_close(view))
            ok = false;
        return ok;
    }
}

uint64_t egtb_maximum_index(const Egtb *egtb)
{
    return egtb == NULL ? 0 : egtb->maximum_index;
}

uint64_t egtb_page_count(const Egtb *egtb)
{
    return egtb == NULL ? 0 : egtb->side_page_count;
}

uint32_t egtb_page_size(const Egtb *egtb)
{
    return egtb == NULL ? 0 : egtb->page_size;
}

uint32_t egtb_cache_page_size(const Egtb *egtb)
{
    return egtb == NULL ? 0 : egtb->memory_page_size;
}

uint32_t egtb_positions_per_page(const Egtb *egtb)
{
    return egtb == NULL ? 0 : egtb->entries_per_page;
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
        (egtb->maximum_index + 1) *
        (egtb->format_version == EGTB_FORMAT_VERSION ? 4u : 2u);

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
    if (!create_with_version(&target, temporary, source->maximum_index,
                             source->page_size, &options,
                             source->format_version))
        goto done;
    egtb_progress_begin("compaction", source->page_count, "pages");
    target->exact_layout = true;
    for (page = 0; page < source->page_count; ++page) {
        size_t cache_index;
        if (!cached_page(source, page, &cache_index) ||
            !store_page(target, page, cache_data(source, cache_index), true))
            goto done;
        egtb_progress_add(1);
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
    egtb_progress_end(ok);
    if (target != NULL)
        egtb_close(target);
    if (source != NULL)
        destroy_egtb(source);
    if (!ok && temporary != NULL)
        unlink(temporary);
    free(temporary);
    return ok;
}
