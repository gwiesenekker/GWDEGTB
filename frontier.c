#define _POSIX_C_SOURCE 200809L

#include "frontier.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zstd.h>

#define FRONTIER_DISTANCE_COUNT ((unsigned)EGTB_MAX_LOSS_DTM + 1u)
#define FRONTIER_BLOCK_RECORDS 512u

typedef struct {
    uint64_t offset;
    uint32_t compressed_size;
    uint32_t checksum;
    uint16_t record_count;
} FrontierBlock;

typedef struct {
    uint64_t *pending;
    unsigned pending_count;
    FrontierBlock *blocks;
    size_t block_count;
    size_t block_capacity;
    uint64_t record_count;
} FrontierStream;

typedef struct {
    int descriptor;
    uint64_t end_offset;
    void *compressed;
    size_t compressed_capacity;
    uint64_t records[FRONTIER_BLOCK_RECORDS];
} FrontierOwner;

struct FrontierStore {
    unsigned owner_count;
    int compression_level;
    uint16_t maximum_distance;
    FrontierOwner *owners;
    FrontierStream *streams;
};

static char frontier_error[256];

static bool frontier_fail(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(frontier_error, sizeof(frontier_error), format, arguments);
    va_end(arguments);
    return false;
}

const char *frontier_last_error(void)
{
    return frontier_error;
}

static unsigned distance_of(int16_t dtm)
{
    return dtm < 0 ? (unsigned)-dtm : (unsigned)dtm;
}

static bool valid_frontier_dtm(int16_t dtm)
{
    return dtm != EGTB_DRAW &&
           ((dtm > 0 && dtm <= EGTB_MAX_WIN_DTM && (dtm & 1) != 0) ||
            (dtm <= 0 && dtm >= -EGTB_MAX_LOSS_DTM && (dtm & 1) == 0));
}

static size_t stream_index(const FrontierStore *store, unsigned owner,
                           EgtbSide side, unsigned distance)
{
    return (((size_t)side * FRONTIER_DISTANCE_COUNT + distance) *
            store->owner_count) + owner;
}

static FrontierStream *get_stream(FrontierStore *store, unsigned owner,
                                  EgtbSide side, unsigned distance)
{
    return &store->streams[stream_index(store, owner, side, distance)];
}

static const FrontierStream *get_const_stream(const FrontierStore *store,
                                              unsigned owner, EgtbSide side,
                                              unsigned distance)
{
    return &store->streams[stream_index(store, owner, side, distance)];
}

static uint32_t checksum32(const void *data, size_t size)
{
    const unsigned char *bytes = data;
    uint32_t hash = UINT32_C(2166136261);
    while (size-- != 0) {
        hash ^= *bytes++;
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static bool write_all_at(int descriptor, uint64_t offset, const void *data,
                         size_t size)
{
    const unsigned char *bytes = data;
    while (size != 0) {
        ssize_t written = pwrite(descriptor, bytes, size, (off_t)offset);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (written == 0)
            return false;
        bytes += (size_t)written;
        size -= (size_t)written;
        offset += (uint64_t)written;
    }
    return true;
}

static bool read_all_at(int descriptor, uint64_t offset, void *data,
                        size_t size)
{
    unsigned char *bytes = data;
    while (size != 0) {
        ssize_t count = pread(descriptor, bytes, size, (off_t)offset);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (count == 0)
            return false;
        bytes += (size_t)count;
        size -= (size_t)count;
        offset += (uint64_t)count;
    }
    return true;
}

static bool append_block(FrontierStore *store, unsigned owner_index,
                         FrontierStream *stream)
{
    FrontierOwner *owner = &store->owners[owner_index];
    FrontierBlock *block;
    size_t source_size, compressed_size;
    if (stream->pending_count == 0)
        return true;
    if (stream->block_count == stream->block_capacity) {
        size_t capacity = stream->block_capacity == 0
                              ? 8 : stream->block_capacity * 2;
        FrontierBlock *blocks;
        if (capacity > SIZE_MAX / sizeof(*blocks))
            return frontier_fail("frontier block directory is too large");
        blocks = realloc(stream->blocks, capacity * sizeof(*blocks));
        if (blocks == NULL)
            return frontier_fail("cannot grow frontier block directory");
        stream->blocks = blocks;
        stream->block_capacity = capacity;
    }
    source_size = stream->pending_count * sizeof(*stream->pending);
    compressed_size = ZSTD_compress(owner->compressed,
                                    owner->compressed_capacity,
                                    stream->pending, source_size,
                                    store->compression_level);
    if (ZSTD_isError(compressed_size) || compressed_size > UINT32_MAX)
        return frontier_fail("cannot compress frontier block: %s",
                             ZSTD_getErrorName(compressed_size));
    if (!write_all_at(owner->descriptor, owner->end_offset,
                      owner->compressed, compressed_size))
        return frontier_fail("cannot write frontier block: %s",
                             strerror(errno));
    block = &stream->blocks[stream->block_count++];
    block->offset = owner->end_offset;
    block->compressed_size = (uint32_t)compressed_size;
    block->checksum = checksum32(stream->pending, source_size);
    block->record_count = (uint16_t)stream->pending_count;
    owner->end_offset += compressed_size;
    stream->pending_count = 0;
    return true;
}

bool frontier_store_create(FrontierStore **out, unsigned owner_count,
                           int compression_level)
{
    FrontierStore *store;
    size_t stream_count;
    unsigned owner;
    if (out == NULL || owner_count == 0)
        return frontier_fail("invalid frontier store arguments");
    *out = NULL;
    stream_count = (size_t)2 * FRONTIER_DISTANCE_COUNT * owner_count;
    if (stream_count > SIZE_MAX / sizeof(FrontierStream))
        return frontier_fail("frontier stream table is too large");
    store = calloc(1, sizeof(*store));
    if (store == NULL)
        return frontier_fail("cannot allocate frontier store");
    store->owner_count = owner_count;
    store->compression_level = compression_level;
    store->owners = calloc(owner_count, sizeof(*store->owners));
    store->streams = calloc(stream_count, sizeof(*store->streams));
    if (store->owners == NULL || store->streams == NULL) {
        frontier_store_destroy(store);
        return frontier_fail("cannot allocate frontier store tables");
    }
    for (owner = 0; owner < owner_count; ++owner)
        store->owners[owner].descriptor = -1;
    for (owner = 0; owner < owner_count; ++owner) {
        char path[] = ".gwdegtb-frontier-XXXXXX";
        FrontierOwner *with = &store->owners[owner];
        with->descriptor = mkstemp(path);
        if (with->descriptor < 0) {
            frontier_store_destroy(store);
            return frontier_fail("cannot create frontier temporary file: %s",
                                 strerror(errno));
        }
        unlink(path);
        with->compressed_capacity =
            ZSTD_compressBound(FRONTIER_BLOCK_RECORDS * sizeof(uint64_t));
        with->compressed = malloc(with->compressed_capacity);
        if (with->compressed == NULL) {
            frontier_store_destroy(store);
            return frontier_fail("cannot allocate frontier compression buffer");
        }
    }
    *out = store;
    return true;
}

void frontier_store_destroy(FrontierStore *store)
{
    unsigned owner;
    if (store == NULL)
        return;
    if (store->streams != NULL)
        for (unsigned side = 0; side < 2; ++side)
            for (unsigned distance = 0; distance <= store->maximum_distance; ++distance)
                for (owner = 0; owner < store->owner_count; ++owner) {
                    FrontierStream *stream = get_stream(store, owner,
                                                        (EgtbSide)side, distance);
                    free(stream->pending);
                    free(stream->blocks);
                }
    if (store->owners != NULL)
        for (owner = 0; owner < store->owner_count; ++owner) {
            if (store->owners[owner].descriptor >= 0)
                close(store->owners[owner].descriptor);
            free(store->owners[owner].compressed);
        }
    free(store->streams);
    free(store->owners);
    free(store);
}

bool frontier_store_append(FrontierStore *store, unsigned owner,
                           EgtbSide side, int16_t dtm, uint64_t index)
{
    FrontierStream *stream;
    unsigned distance;
    if (store == NULL || owner >= store->owner_count ||
        (side != EGTB_WHITE_TO_MOVE && side != EGTB_BLACK_TO_MOVE) ||
        !valid_frontier_dtm(dtm))
        return frontier_fail("invalid frontier append");
    distance = distance_of(dtm);
    stream = get_stream(store, owner, side, distance);
    if (stream->pending == NULL) {
        stream->pending = malloc(FRONTIER_BLOCK_RECORDS *
                                 sizeof(*stream->pending));
        if (stream->pending == NULL)
            return frontier_fail("cannot allocate frontier write buffer");
    }
    stream->pending[stream->pending_count++] = index;
    ++stream->record_count;
    {
        uint16_t observed = __atomic_load_n(&store->maximum_distance,
                                            __ATOMIC_RELAXED);
        while (distance > observed &&
               !__atomic_compare_exchange_n(
                   &store->maximum_distance, &observed, (uint16_t)distance,
                   false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            ;
    }
    if (stream->pending_count == FRONTIER_BLOCK_RECORDS &&
        !append_block(store, owner, stream))
        return false;
    return true;
}

bool frontier_store_finish(FrontierStore *store)
{
    unsigned side, distance, owner;
    if (store == NULL)
        return frontier_fail("invalid frontier store");
    for (side = 0; side < 2; ++side)
        for (distance = 0; distance <= store->maximum_distance; ++distance)
            for (owner = 0; owner < store->owner_count; ++owner) {
                FrontierStream *stream = get_stream(
                    store, owner, (EgtbSide)side, distance);
                if (!append_block(store, owner, stream))
                    return false;
            }
    return true;
}

bool frontier_store_visit(FrontierStore *store, unsigned owner,
                          EgtbSide side, int16_t dtm,
                          FrontierVisitor visitor, void *context)
{
    FrontierOwner *file;
    FrontierStream *stream;
    unsigned distance;
    size_t block_index;
    if (store == NULL || owner >= store->owner_count || visitor == NULL ||
        !valid_frontier_dtm(dtm))
        return frontier_fail("invalid frontier visit");
    distance = distance_of(dtm);
    stream = get_stream(store, owner, side, distance);
    if (!append_block(store, owner, stream))
        return false;
    file = &store->owners[owner];
    for (block_index = 0; block_index < stream->block_count; ++block_index) {
        const FrontierBlock *block = &stream->blocks[block_index];
        size_t output_size = block->record_count * sizeof(uint64_t);
        size_t decompressed;
        unsigned record;
        if (block->compressed_size > file->compressed_capacity ||
            !read_all_at(file->descriptor, block->offset, file->compressed,
                         block->compressed_size))
            return frontier_fail("cannot read frontier block: %s",
                                 strerror(errno));
        decompressed = ZSTD_decompress(file->records, sizeof(file->records),
                                       file->compressed,
                                       block->compressed_size);
        if (ZSTD_isError(decompressed) || decompressed != output_size)
            return frontier_fail("cannot decompress frontier block: %s",
                                 ZSTD_getErrorName(decompressed));
        if (checksum32(file->records, output_size) != block->checksum)
            return frontier_fail("frontier block checksum mismatch");
        for (record = 0; record < block->record_count; ++record)
            if (!visitor(file->records[record], context))
                return false;
    }
    return true;
}

uint64_t frontier_store_count(const FrontierStore *store, EgtbSide side,
                              int16_t dtm)
{
    uint64_t count = 0;
    unsigned owner, distance;
    if (store == NULL || !valid_frontier_dtm(dtm))
        return 0;
    distance = distance_of(dtm);
    for (owner = 0; owner < store->owner_count; ++owner)
        count += get_const_stream(store, owner, side, distance)->record_count;
    return count;
}

uint16_t frontier_store_maximum_distance(const FrontierStore *store)
{
    return store == NULL ? 0 :
        __atomic_load_n(&store->maximum_distance, __ATOMIC_RELAXED);
}
