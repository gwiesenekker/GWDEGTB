#define _POSIX_C_SOURCE 200809L

#include "wdl.h"
#include "crc32c.h"
#include <stdatomic.h>

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
#include <zdict.h>

#define WDL_HEADER_SIZE 64
#define WDL_DIRECTORY_ENTRY_SIZE 14
#define WDL_POSITIONS_PER_PAGE (WDL_PAGE_SIZE * 2)
#define WDL_INVALID_PAGE UINT64_MAX
#define WDL_MAX_DICTIONARY_BYTES (256u * 1024u)
#ifndef WDL_DICTIONARY_SAMPLES
#define WDL_DICTIONARY_SAMPLES 4096u
#endif

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
    ZSTD_DDict *dictionary;
    unsigned char *compressed;
    size_t compressed_capacity;
};

struct WdlImage {
    const unsigned char *data;
    size_t size;
    uint64_t maximum_index;
    uint64_t page_count;
    uint64_t packed_bytes;
    uint64_t data_offset;
    ZSTD_DDict *dictionary;
};

static const unsigned char wdl_magic[8] = {'I','P','D','W','D','L','\0','\0'};
static _Thread_local char last_error[256];

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

/* A worker buffers at most 1024 compressed pages (~1 MiB), then reserves
 * one exact output extent. Page-directory ranges are disjoint. */
#define WDL_BATCH_PAGES 1024

typedef struct {
    Egtb *dtm;
    int descriptor;
    int level;
    ZSTD_CDict *dictionary;
    uint64_t positions;
    uint64_t next_offset;
    pthread_mutex_t mutex;
    atomic_bool cancelled;
} WdlCompileShared;

typedef struct {
    WdlCompileShared *shared;
    uint64_t first_page, end_page;
    WdlStatistics statistics;
    WdlStorageStatistics storage;
    bool failed;
    char error[256];
} WdlCompileWorker;

static bool pwrite_all(int fd, uint64_t offset, const void *data, size_t bytes)
{
    const unsigned char *p = data;
    if (offset > INT64_MAX || bytes > (uint64_t)INT64_MAX - offset)
        return fail("WDL output offset overflow");
    while (bytes) {
        ssize_t n = pwrite(fd, p, bytes, (off_t)offset);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return fail("WDL file write failed: %s",
                        n < 0 ? strerror(errno) : "short write");
        p += n;
        offset += (size_t)n;
        bytes -= (size_t)n;
    }
    return true;
}

static void *compile_wdl_pages(void *opaque)
{
    WdlCompileWorker *w = opaque;
    WdlCompileShared *s = w->shared;
    EgtbView *view = NULL;
    EgtbSequentialReader reader;
    ZSTD_CCtx *compressor = ZSTD_createCCtx();
    size_t bound = ZSTD_compressBound(WDL_PAGE_SIZE);
    unsigned char *payload = malloc(bound * WDL_BATCH_PAGES);
    unsigned char *directory = malloc(WDL_DIRECTORY_ENTRY_SIZE * WDL_BATCH_PAGES);
    unsigned char decoded[WDL_PAGE_SIZE];
    uint64_t first = w->first_page * WDL_POSITIONS_PER_PAGE;
    uint64_t end = w->end_page * WDL_POSITIONS_PER_PAGE;
    if (end > s->positions)
        end = s->positions;
    if (!compressor || !payload || !directory) {
        fail("cannot allocate WDL compilation worker");
        goto failed;
    }
    /* One page per plane; the cursor consumes both before advancing. */
    if (!egtb_view_create(&view, s->dtm, 2, false) ||
        !egtb_sequential_reader_init(&reader, view, first, end)) {
        fail("cannot create DTM sequential reader: %s", egtb_last_error());
        goto failed;
    }
    for (uint64_t batch = w->first_page; batch < w->end_page;) {
        size_t pages = (size_t)(w->end_page - batch);
        size_t used = 0;
        uint64_t offset;
        if (atomic_load_explicit(&s->cancelled, memory_order_relaxed))
            goto done;
        if (pages > WDL_BATCH_PAGES)
            pages = WDL_BATCH_PAGES;
        memset(directory, 0, pages * WDL_DIRECTORY_ENTRY_SIZE);
        for (size_t p = 0; p < pages; ++p) {
            uint64_t index = (batch + p) * WDL_POSITIONS_PER_PAGE;
            size_t count = (size_t)(s->positions - index);
            bool all_draw = true;
            if (count > WDL_POSITIONS_PER_PAGE)
                count = WDL_POSITIONS_PER_PAGE;
            memset(decoded, 0, sizeof(decoded));
            for (size_t i = 0; i < count; ++i) {
                int16_t white, black;
                if (!egtb_sequential_reader_next(&reader, &white, &black)) {
                    fail("cannot read DTM source: %s", egtb_last_error());
                    goto failed;
                }
                WdlResult a = dtm_to_wdl(white), b = dtm_to_wdl(black);
                unsigned char packed = (unsigned char)(a | (b << 2));
                decoded[i / 2] |= (unsigned char)(packed << ((i % 2) * 4));
                all_draw &= packed == 0;
                count_result(&w->statistics, EGTB_WHITE_TO_MOVE, a);
                count_result(&w->statistics, EGTB_BLACK_TO_MOVE, b);
            }
            if (!all_draw) {
                size_t n = s->dictionary
                    ? ZSTD_compress_usingCDict(compressor, payload + used, bound,
                                              decoded, sizeof(decoded), s->dictionary)
                    : ZSTD_compressCCtx(compressor, payload + used, bound,
                                         decoded, sizeof(decoded), s->level);
                if (ZSTD_isError(n) || !n || n > UINT16_MAX) {
                    fail("WDL Zstd compression failed: %s", ZSTD_getErrorName(n));
                    goto failed;
                }
                unsigned char *entry = directory + p * WDL_DIRECTORY_ENTRY_SIZE;
                put_u64(entry, used); /* rebased after reserving the batch */
                put_u16(entry + 8, (uint16_t)n);
                put_u32(entry + 10, crc32c(decoded, sizeof(decoded)));
                used += n;
                ++w->storage.stored_pages;
            }
        }
        pthread_mutex_lock(&s->mutex);
        offset = s->next_offset;
        bool fits = offset <= INT64_MAX && used <= (uint64_t)INT64_MAX - offset;
        if (fits)
            s->next_offset += used;
        pthread_mutex_unlock(&s->mutex);
        if (!fits) {
            fail("WDL output offset overflow");
            goto failed;
        }
        for (size_t p = 0; p < pages; ++p) {
            unsigned char *entry = directory + p * WDL_DIRECTORY_ENTRY_SIZE;
            if (get_u16(entry + 8))
                put_u64(entry, get_u64(entry) + offset);
        }
        if (!pwrite_all(s->descriptor, offset, payload, used) ||
            !pwrite_all(s->descriptor,
                         WDL_HEADER_SIZE + batch * WDL_DIRECTORY_ENTRY_SIZE,
                         directory, pages * WDL_DIRECTORY_ENTRY_SIZE))
            goto failed;
        w->storage.compressed_payload_bytes += used;
        batch += pages;
    }
    goto done;
failed:
    w->failed = true;
    snprintf(w->error, sizeof(w->error), "%s", wdl_last_error());
    atomic_store_explicit(&s->cancelled, true, memory_order_relaxed);
done:
    if (!egtb_view_close(view) && !w->failed) {
        w->failed = true;
        snprintf(w->error, sizeof(w->error), "cannot close DTM view: %s",
                 egtb_last_error());
        atomic_store_explicit(&s->cancelled, true, memory_order_relaxed);
    }
    ZSTD_freeCCtx(compressor);
    free(payload);
    free(directory);
    return NULL;
}

unsigned wdl_default_threads(void)
{
    const char *value = getenv("EGTB_WDL_THREADS");
    char *end;
    unsigned long n;
    if (!value || !*value)
        return 4;
    errno = 0;
    n = strtoul(value, &end, 10);
    if (errno || *end || n == 0 || n > WDL_MAX_DECOMPRESSION_THREADS) {
        fail("EGTB_WDL_THREADS must be between 1 and %u",
             WDL_MAX_DECOMPRESSION_THREADS);
        return 0;
    }
    return (unsigned)n;
}

static bool train_dictionary(Egtb *dtm, uint64_t positions, uint64_t pages,
                              unsigned char **out, size_t *out_size)
{
    const char *setting = getenv("EGTB_WDL_DICTIONARY_KIB");
    size_t capacity = 110u * 1024u;
    *out = NULL;
    *out_size = 0;
    if (setting && *setting) {
        char *end;
        errno = 0;
        unsigned long n = strtoul(setting, &end, 10);
        if (errno || *end || *setting < '0' || *setting > '9' || n > 256)
            return fail("EGTB_WDL_DICTIONARY_KIB must be 0..256");
        capacity = (size_t)n * 1024;
    }
    if (!capacity || pages < 256)
        return true;
    unsigned attempts = pages < WDL_DICTIONARY_SAMPLES
                            ? (unsigned)pages : WDL_DICTIONARY_SAMPLES;
    unsigned char *samples = malloc((size_t)attempts * WDL_PAGE_SIZE);
    size_t *sizes = malloc((size_t)attempts * sizeof(*sizes));
    unsigned char *dictionary = NULL;
    EgtbView *view = NULL;
    unsigned count = 0;
    bool ok = false;
    if (!samples || !sizes) {
        fail("cannot allocate WDL dictionary samples");
        goto done;
    }
    if (!egtb_view_create(&view, dtm, 2, false)) {
        fail("cannot create dictionary sampling view: %s", egtb_last_error());
        goto done;
    }
    for (unsigned i = 0; i < attempts; ++i) {
        /* One deterministic pseudorandom page per equal index stratum;
         * avoids a fixed stride repeatedly sampling the same pattern. */
        uint64_t first = (pages / attempts) * i + (pages % attempts) * i / attempts;
        uint64_t end = (pages / attempts) * (i + 1) +
                       (pages % attempts) * (i + 1) / attempts;
        uint64_t hash = (uint64_t)(i + 1) * UINT64_C(0x9e3779b97f4a7c15);
        hash = (hash ^ (hash >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
        hash = (hash ^ (hash >> 27)) * UINT64_C(0x94d049bb133111eb);
        uint64_t page = first + (hash ^ (hash >> 31)) % (end - first);
        uint64_t start = page * WDL_POSITIONS_PER_PAGE;
        uint64_t limit = positions - start;
        if (limit > WDL_POSITIONS_PER_PAGE) limit = WDL_POSITIONS_PER_PAGE;
        EgtbSequentialReader reader;
        if (!egtb_sequential_reader_init(&reader, view, start, start + limit)) {
            fail("cannot initialize WDL dictionary sample: %s", egtb_last_error());
            goto done;
        }
        unsigned char *sample = samples + (size_t)count * WDL_PAGE_SIZE;
        memset(sample, 0, WDL_PAGE_SIZE);
        bool nonzero = false;
        for (uint64_t j = 0; j < limit; ++j) {
            int16_t white, black;
            if (!egtb_sequential_reader_next(&reader, &white, &black)) {
                fail("cannot read WDL dictionary sample: %s", egtb_last_error());
                goto done;
            }
            unsigned packed = dtm_to_wdl(white) | (dtm_to_wdl(black) << 2);
            sample[j / 2] |= (unsigned char)(packed << ((j % 2) * 4));
            nonzero |= packed != 0;
        }
        if (nonzero) sizes[count++] = WDL_PAGE_SIZE;
    }
    if (count < 128) { ok = true; goto done; }
    /* Avoid oversized dictionaries on small or draw-heavy databases. */
    size_t sample_bytes = (size_t)count * WDL_PAGE_SIZE;
    if (capacity > sample_bytes / 32) capacity = sample_bytes / 32;
    dictionary = malloc(capacity);
    if (!dictionary) { fail("cannot allocate WDL dictionary"); goto done; }
    size_t trained = ZDICT_trainFromBuffer(dictionary, capacity, samples, sizes, count);
    if (ZDICT_isError(trained)) {
        fprintf(stderr, "WDL dictionary training skipped: %s\n", ZDICT_getErrorName(trained));
        ok = true;
        goto done;
    }
    printf("WDL dictionary: %zu bytes trained from %u non-draw pages\n", trained, count);
    *out = dictionary;
    *out_size = trained;
    dictionary = NULL;
    ok = true;
done:
    if (!egtb_view_close(view) && ok) {
        fail("cannot close WDL dictionary sample view: %s", egtb_last_error());
        ok = false;
    }
    free(dictionary);
    free(samples);
    free(sizes);
    return ok;
}

static bool prepare_dictionary(const void *bytes, size_t size, uint32_t checksum,
                                ZSTD_DDict **out)
{
    *out = NULL;
    if (crc32c(bytes, size) != checksum)
        return fail("CRC32C mismatch for WDL dictionary");
    if (!ZSTD_getDictID_fromDict(bytes, size))
        return fail("invalid trained WDL dictionary");
    *out = ZSTD_createDDict(bytes, size);
    return *out != NULL || fail("cannot prepare WDL dictionary");
}

bool wdl_compile_threaded(const char *dtm_path, const char *wdl_path,
                           int compression_level, size_t dtm_cache_pages,
                           unsigned thread_count, WdlStatistics *statistics,
                           WdlStorageStatistics *storage_statistics)
{
    Egtb *dtm = NULL;
    WdlCompileWorker *workers = NULL;
    pthread_t *threads = NULL;
    unsigned created = 0;
    unsigned char header[WDL_HEADER_SIZE] = {0};
    char *temporary = NULL;
    bool ok = false, mutex_ready = false, temporary_created = false;
    WdlCompileShared shared = {0};
    WdlStatistics totals = {0};
    WdlStorageStatistics storage = {0};
    unsigned char *dictionary = NULL;
    size_t dictionary_size = 0;
    uint64_t pages, packed_bytes, data_offset;
    shared.descriptor = -1;
    atomic_init(&shared.cancelled, false);
    if (!dtm_path || !wdl_path || !dtm_cache_pages ||
        !thread_count || thread_count > WDL_MAX_DECOMPRESSION_THREADS)
        return fail("invalid WDL compilation argument");
    /* The legacy cache argument remains accepted; streaming needs only a
     * two-page private view per worker, not a large random-access cache. */
    if (!egtb_open_readonly(&dtm, dtm_path, 1))
        return fail("cannot open DTM source: %s", egtb_last_error());
    shared.dtm = dtm;
    shared.positions = egtb_maximum_index(dtm) + 1;
    shared.level = compression_level;
    packed_bytes = shared.positions / 2 + (shared.positions % 2 != 0);
    pages = packed_bytes / WDL_PAGE_SIZE + (packed_bytes % WDL_PAGE_SIZE != 0);
    if (!shared.positions ||
        pages > ((uint64_t)INT64_MAX - WDL_HEADER_SIZE) / WDL_DIRECTORY_ENTRY_SIZE) {
        fail("WDL output is too large");
        goto done;
    }
    if (!train_dictionary(dtm, shared.positions, pages, &dictionary, &dictionary_size))
        goto done;
    if (dictionary_size) {
        shared.dictionary = ZSTD_createCDict(dictionary, dictionary_size, compression_level);
        if (!shared.dictionary) { fail("cannot prepare WDL compression dictionary"); goto done; }
    }
    data_offset = WDL_HEADER_SIZE + pages * WDL_DIRECTORY_ENTRY_SIZE;
    if (dictionary_size > (uint64_t)INT64_MAX - data_offset) {
        fail("WDL dictionary offset overflow"); goto done;
    }
    data_offset += dictionary_size;
    shared.next_offset = data_offset;
    if (thread_count > pages)
        thread_count = (unsigned)pages;
    workers = calloc(thread_count, sizeof(*workers));
    threads = calloc(thread_count, sizeof(*threads));
    temporary = malloc(strlen(wdl_path) + 24);
    if (!workers || !threads || !temporary) {
        fail("cannot allocate WDL compilation data");
        goto done;
    }
    int error = pthread_mutex_init(&shared.mutex, NULL);
    if (error) {
        fail("cannot initialize WDL writer mutex: %s", strerror(error));
        goto done;
    }
    mutex_ready = true;
    snprintf(temporary, strlen(wdl_path) + 24, "%s.tmp.XXXXXX", wdl_path);
    shared.descriptor = mkstemp(temporary);
    if (shared.descriptor < 0) {
        fail("cannot create temporary WDL file: %s", strerror(errno));
        goto done;
    }
    temporary_created = true;
    memcpy(header, wdl_magic, sizeof(wdl_magic));
    header[8] = dictionary_size ? WDL_FORMAT_VERSION : WDL_LEGACY_FORMAT_VERSION;
    put_u16(header + 10, WDL_HEADER_SIZE);
    put_u32(header + 12, WDL_PAGE_SIZE);
    put_u64(header + 16, shared.positions - 1);
    put_u64(header + 24, pages);
    put_u64(header + 32, WDL_HEADER_SIZE);
    put_u64(header + 40, data_offset);
    put_u64(header + 48, packed_bytes);
    put_u32(header + 56, (uint32_t)dictionary_size);
    if (dictionary_size) put_u32(header + 60, crc32c(dictionary, dictionary_size));
    if (ftruncate(shared.descriptor, (off_t)data_offset) != 0) {
        fail("cannot initialize WDL output: %s", strerror(errno));
        goto done;
    }
    if (!pwrite_all(shared.descriptor, 0, header, sizeof(header)))
        goto done;
    if (dictionary_size && !pwrite_all(shared.descriptor,
            data_offset - dictionary_size, dictionary, dictionary_size)) goto done;
    for (unsigned i = 0; i < thread_count; ++i) {
        uint64_t quotient = pages / thread_count, remainder = pages % thread_count;
        workers[i].shared = &shared;
        workers[i].first_page = i * quotient + (i < remainder ? i : remainder);
        workers[i].end_page = workers[i].first_page + quotient + (i < remainder);
        error = pthread_create(&threads[i], NULL, compile_wdl_pages, &workers[i]);
        if (error) {
            fail("cannot create WDL compilation worker: %s", strerror(error));
            atomic_store_explicit(&shared.cancelled, true, memory_order_relaxed);
            break;
        }
        ++created;
    }
    for (unsigned i = 0; i < created; ++i)
        pthread_join(threads[i], NULL);
    if (created != thread_count)
        goto done;
    for (unsigned i = 0; i < thread_count; ++i) {
        if (workers[i].failed) {
            fail("WDL compilation worker %u failed: %s", i, workers[i].error);
            goto done;
        }
        for (unsigned side = 0; side < 2; ++side) {
            totals.wins[side] += workers[i].statistics.wins[side];
            totals.draws[side] += workers[i].statistics.draws[side];
            totals.losses[side] += workers[i].statistics.losses[side];
        }
        storage.stored_pages += workers[i].storage.stored_pages;
        storage.compressed_payload_bytes += workers[i].storage.compressed_payload_bytes;
    }
    if (!egtb_close(dtm)) {
        dtm = NULL;
        fail("cannot close DTM source: %s", egtb_last_error());
        goto done;
    }
    dtm = NULL;
    if (fsync(shared.descriptor) != 0) {
        fail("cannot flush WDL output: %s", strerror(errno));
        goto done;
    }
    if (close(shared.descriptor) != 0) {
        shared.descriptor = -1;
        fail("cannot close WDL output: %s", strerror(errno));
        goto done;
    }
    shared.descriptor = -1;
    if (!egtb_publish(temporary, wdl_path)) {
        fail("cannot publish WDL output: %s", egtb_last_error());
        goto done;
    }
    storage.logical_uncompressed_bytes = packed_bytes;
    storage.file_bytes = shared.next_offset;
    if (statistics)
        *statistics = totals;
    if (storage_statistics)
        *storage_statistics = storage;
    ok = true;
done:
    if (shared.descriptor >= 0)
        close(shared.descriptor);
    if (!ok && temporary_created)
        unlink(temporary);
    if (dtm)
        egtb_close(dtm);
    if (mutex_ready)
        pthread_mutex_destroy(&shared.mutex);
    free(temporary);
    free(threads);
    free(workers);
    ZSTD_freeCDict(shared.dictionary);
    free(dictionary);
    return ok;
}

bool wdl_compile(const char *dtm_path, const char *wdl_path,
                 int compression_level, size_t dtm_cache_pages,
                 WdlStatistics *statistics,
                 WdlStorageStatistics *storage_statistics)
{
    return wdl_compile_threaded(dtm_path, wdl_path, compression_level,
                                dtm_cache_pages, 1, statistics,
                                storage_statistics);
}

static void destroy_wdl(Wdl *wdl)
{
    if (wdl == NULL)
        return;
    if (wdl->file != NULL)
        fclose(wdl->file);
    ZSTD_freeDCtx(wdl->decompressor);
    ZSTD_freeDDict(wdl->dictionary);
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
    if (header[8] != WDL_FORMAT_VERSION && header[8] != WDL_LEGACY_FORMAT_VERSION) {
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
    uint32_t dictionary_size = header[8] == WDL_FORMAT_VERSION ? get_u32(header + 56) : 0;
    if (header[8] == WDL_FORMAT_VERSION &&
        (!dictionary_size || dictionary_size > WDL_MAX_DICTIONARY_BYTES)) {
        fail("invalid WDL dictionary size"); goto failure;
    }
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
        wdl->page_count > (UINT64_MAX - WDL_HEADER_SIZE - dictionary_size) /
                              WDL_DIRECTORY_ENTRY_SIZE ||
        wdl->data_offset != WDL_HEADER_SIZE +
                                wdl->page_count * WDL_DIRECTORY_ENTRY_SIZE + dictionary_size ||
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
    if (dictionary_size) {
        unsigned char *dictionary = malloc(dictionary_size);
        if (!dictionary) { fail("cannot allocate WDL dictionary"); goto failure; }
        bool valid = read_at(wdl->file, wdl->data_offset - dictionary_size,
                             dictionary, dictionary_size) &&
                     prepare_dictionary(dictionary, dictionary_size,
                                         get_u32(header + 60), &wdl->dictionary);
        free(dictionary);
        if (!valid) goto failure;
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
            } else if (wdl->lengths[current] > ZSTD_compressBound(WDL_PAGE_SIZE) ||
                       (uint64_t)wdl->lengths[current] > wdl->file_bytes ||
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
    unsigned threads = wdl_default_threads();
    if (!threads) {
        if (out) *out = NULL;
        return false;
    }
    return wdl_open_threaded(out, path, cache_pages, compression_level,
                             dtm_cache_pages, threads);
}

bool wdl_open_threaded(Wdl **out, const char *path, size_t cache_pages,
                       int compression_level, size_t dtm_cache_pages,
                       unsigned thread_count)
{
    bool not_found;
    char *dtm_path = NULL;
    WdlStatistics statistics;
    WdlStorageStatistics storage;
    bool ok;
    if (out != NULL) *out = NULL;
    if (out == NULL || path == NULL || cache_pages == 0 ||
        dtm_cache_pages == 0 || thread_count == 0 ||
        thread_count > WDL_MAX_DECOMPRESSION_THREADS)
        return fail("invalid WDL open argument");
    if (open_existing(out, path, cache_pages, &not_found))
        return true;
    if (!not_found)
        return false;
    if (!replace_extension(path, ".wdl", ".dtm", &dtm_path))
        return false;
    printf("WDL generation: %s with up to %u threads (Zstd level %d)\n",
           path, thread_count, compression_level);
    fflush(stdout);
    ok = wdl_compile_threaded(dtm_path, path, compression_level, dtm_cache_pages,
                              thread_count, &statistics, &storage);
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
    entry->page_number = UINT64_MAX;
    if (wdl->offsets[page] == 0) {
        memset(entry->data, 0, sizeof(entry->data));
    } else {
        uint16_t length = wdl->lengths[page];
        if (length > wdl->compressed_capacity ||
            !read_at(wdl->file, wdl->offsets[page], wdl->compressed, length))
            return false;
        decompressed = ZSTD_decompress_usingDDict(
            wdl->decompressor, entry->data, sizeof(entry->data),
            wdl->compressed, length, wdl->dictionary);
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

bool wdl_decompress_into(Wdl *wdl, void *data, size_t size)
{
    return wdl_decompress_into_threaded(wdl, data, size, 1);
}

typedef struct {
    const Wdl *wdl;
    unsigned char *bitmap;
    uint64_t first_page;
    uint64_t end_page;
    bool failed;
    char error[256];
} WdlDecompressWorker;

static bool pread_worker(int descriptor, uint64_t offset, void *data,
                         size_t size, char *error, size_t error_size)
{
    unsigned char *destination = data;
    while (size != 0) {
        ssize_t got = pread(descriptor, destination, size, (off_t)offset);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0) {
            snprintf(error, error_size, "WDL file read failed or was truncated: %s",
                     got < 0 ? strerror(errno) : "unexpected end of file");
            return false;
        }
        destination += (size_t)got;
        offset += (uint64_t)got;
        size -= (size_t)got;
    }
    return true;
}

static void *decompress_wdl_pages(void *opaque)
{
    WdlDecompressWorker *worker = opaque;
    const Wdl *wdl = worker->wdl;
    ZSTD_DCtx *decompressor = ZSTD_createDCtx();
    unsigned char *compressed = malloc(ZSTD_compressBound(WDL_PAGE_SIZE));
    unsigned char decoded[WDL_PAGE_SIZE];
    int descriptor = fileno(wdl->file);
    if (decompressor == NULL || compressed == NULL) {
        snprintf(worker->error, sizeof(worker->error),
                 "cannot allocate WDL decompression workspace");
        worker->failed = true;
        goto done;
    }
    for (uint64_t page = worker->first_page; page < worker->end_page; ++page) {
        uint64_t output_offset = page * WDL_PAGE_SIZE;
        size_t output_bytes = WDL_PAGE_SIZE;
        if (output_bytes > wdl->packed_bytes - output_offset)
            output_bytes = (size_t)(wdl->packed_bytes - output_offset);
        if (wdl->offsets[page] == 0) {
            memset(worker->bitmap + output_offset, 0, output_bytes);
            continue;
        }
        uint16_t length = wdl->lengths[page];
        size_t decompressed;
        if (!pread_worker(descriptor, wdl->offsets[page], compressed, length,
                          worker->error, sizeof(worker->error))) {
            worker->failed = true;
            break;
        }
        decompressed = ZSTD_decompress_usingDDict(decompressor, decoded,
                                            sizeof(decoded), compressed,
                                            length, wdl->dictionary);
        if (ZSTD_isError(decompressed)) {
            snprintf(worker->error, sizeof(worker->error),
                     "WDL Zstd decompression failed for page %" PRIu64 ": %s",
                     page, ZSTD_getErrorName(decompressed));
            worker->failed = true;
            break;
        }
        if (decompressed != sizeof(decoded)) {
            snprintf(worker->error, sizeof(worker->error),
                     "decompressed WDL page %" PRIu64 " has an invalid size",
                     page);
            worker->failed = true;
            break;
        }
        if (crc32c(decoded, sizeof(decoded)) != wdl->checksums[page]) {
            snprintf(worker->error, sizeof(worker->error),
                     "CRC32C mismatch for WDL page %" PRIu64, page);
            worker->failed = true;
            break;
        }
        memcpy(worker->bitmap + output_offset, decoded, output_bytes);
    }
done:
    free(compressed);
    ZSTD_freeDCtx(decompressor);
    return NULL;
}

bool wdl_decompress_into_threaded(Wdl *wdl, void *data, size_t size,
                                  unsigned thread_count)
{
    WdlDecompressWorker *workers = NULL;
    pthread_t *threads = NULL;
    unsigned created = 0;
    bool ok = false;

    if (wdl == NULL || data == NULL || wdl->packed_bytes > SIZE_MAX ||
        size != (size_t)wdl->packed_bytes || thread_count == 0 ||
        thread_count > WDL_MAX_DECOMPRESSION_THREADS)
        return fail("invalid resident WDL destination");
    if (thread_count > wdl->page_count)
        thread_count = (unsigned)wdl->page_count;
    if (thread_count == 0)
        thread_count = 1;
    workers = calloc(thread_count, sizeof(*workers));
    threads = calloc(thread_count, sizeof(*threads));
    if (workers == NULL || threads == NULL) {
        fail("cannot allocate WDL decompression workers");
        goto done;
    }
    for (unsigned worker = 0; worker < thread_count; ++worker) {
        uint64_t pages_per_worker = wdl->page_count / thread_count;
        uint64_t extra = wdl->page_count % thread_count;
        workers[worker].wdl = wdl;
        workers[worker].bitmap = data;
        workers[worker].first_page = worker * pages_per_worker +
                                     (worker < extra ? worker : extra);
        workers[worker].end_page = workers[worker].first_page +
                                   pages_per_worker + (worker < extra);
        int error = pthread_create(&threads[worker], NULL,
                                   decompress_wdl_pages, &workers[worker]);
        if (error != 0) {
            fail("cannot create WDL decompression worker %u: %s",
                 worker, strerror(error));
            goto join;
        }
        ++created;
    }
join:
    for (unsigned worker = 0; worker < created; ++worker) {
        int error = pthread_join(threads[worker], NULL);
        if (error != 0) {
            fail("cannot join WDL decompression worker %u: %s",
                 worker, strerror(error));
            goto done;
        }
    }
    if (created != thread_count)
        goto done;
    for (unsigned worker = 0; worker < thread_count; ++worker)
        if (workers[worker].failed) {
            fail("WDL decompression worker %u failed: %s", worker,
                 workers[worker].error);
            goto done;
        }
    ok = true;
done:
    free(threads);
    free(workers);
    return ok;
}

bool wdl_file_size(const char *path, size_t *size)
{
    struct stat status;
    if (path == NULL || size == NULL)
        return fail("invalid WDL file-size argument");
    if (stat(path, &status) != 0)
        return fail("cannot stat %s: %s", path, strerror(errno));
    if (status.st_size < 0 || (uint64_t)status.st_size > SIZE_MAX)
        return fail("WDL file does not fit in address space");
    *size = (size_t)status.st_size;
    return true;
}

typedef struct {
    int descriptor;
    unsigned char *data;
    size_t first, end;
    int error;
    atomic_bool *cancelled;
} WdlLoadWorker;

static void *load_wdl_range(void *argument)
{
    WdlLoadWorker *w = argument;
    size_t offset = w->first;
    while (offset < w->end && !atomic_load_explicit(w->cancelled, memory_order_relaxed)) {
        size_t count = w->end - offset;
        if (count > 1024u * 1024u) count = 1024u * 1024u;
        ssize_t got = pread(w->descriptor, w->data + offset, count, (off_t)offset);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0) {
            w->error = got < 0 ? errno : EIO;
            atomic_store_explicit(w->cancelled, true, memory_order_relaxed);
            break;
        }
        offset += (size_t)got;
    }
    return NULL;
}

bool wdl_file_load_into_threaded(const char *path, void *data, size_t size,
                                 unsigned thread_count)
{
    struct stat status;
    if (!path || !data || !thread_count || thread_count > WDL_MAX_DECOMPRESSION_THREADS)
        return fail("invalid compressed WDL loading argument");
    int descriptor = open(path, O_RDONLY);
    if (descriptor < 0) return fail("cannot open %s: %s", path, strerror(errno));
    bool ok = false;
    WdlLoadWorker *workers = NULL;
    pthread_t *threads = NULL;
    atomic_bool cancelled;
    atomic_init(&cancelled, false);
    if (fstat(descriptor, &status) != 0) {
        fail("cannot stat %s: %s", path, strerror(errno)); goto done;
    }
    if (status.st_size < 0 || (uint64_t)status.st_size != size) {
        fail("invalid compressed WDL destination size"); goto done;
    }
    /* Avoid thread creation overhead on tiny files: at most one per MiB. */
    size_t ranges = size / (1024u * 1024u) + (size % (1024u * 1024u) != 0);
    if (!ranges) { ok = true; goto done; }
    if (thread_count > ranges) thread_count = (unsigned)ranges;
    workers = calloc(thread_count, sizeof(*workers));
    threads = calloc(thread_count, sizeof(*threads));
    if (!workers || !threads) { fail("cannot allocate WDL loading workers"); goto done; }
    unsigned created = 0;
    for (unsigned i = 0; i < thread_count; ++i) {
        size_t quotient = size / thread_count, remainder = size % thread_count;
        workers[i] = (WdlLoadWorker){.descriptor = descriptor, .data = data,
            .first = i * quotient + (i < remainder ? i : remainder),
            .cancelled = &cancelled};
        workers[i].end = workers[i].first + quotient + (i < remainder);
        if (thread_count == 1) { load_wdl_range(&workers[i]); break; }
        int error = pthread_create(&threads[i], NULL, load_wdl_range, &workers[i]);
        if (error) {
            fail("cannot create WDL loading worker: %s", strerror(error));
            atomic_store_explicit(&cancelled, true, memory_order_relaxed);
            break;
        }
        ++created;
    }
    for (unsigned i = 0; i < created; ++i) pthread_join(threads[i], NULL);
    if (thread_count > 1 && created != thread_count) goto done;
    for (unsigned i = 0; i < thread_count; ++i)
        if (workers[i].error) {
            fail("cannot read %s: %s (worker %u)", path, strerror(workers[i].error), i);
            goto done;
        }
    ok = true;
done:
    free(threads);
    free(workers);
    if (close(descriptor) != 0 && ok)
        return fail("cannot close %s: %s", path, strerror(errno));
    return ok;
}

bool wdl_file_load_into(const char *path, void *data, size_t size)
{
    return wdl_file_load_into_threaded(path, data, size, 1);
}

bool wdl_image_attach(WdlImage **out, const void *data, size_t size)
{
    const unsigned char *bytes = data;
    WdlImage *image = NULL;
    uint64_t calculated_pages, directory_offset;
    if (out == NULL || data == NULL || size < WDL_HEADER_SIZE)
        return fail("invalid compressed WDL image");
    *out = NULL;
    if (memcmp(bytes, wdl_magic, sizeof(wdl_magic)) != 0 ||
        (bytes[8] != WDL_FORMAT_VERSION && bytes[8] != WDL_LEGACY_FORMAT_VERSION) ||
        get_u16(bytes + 10) != WDL_HEADER_SIZE ||
        get_u32(bytes + 12) != WDL_PAGE_SIZE)
        return fail("unsupported compressed WDL image");
    image = calloc(1, sizeof(*image));
    if (image == NULL)
        return fail("cannot allocate compressed WDL image handle");
    image->data = bytes;
    image->size = size;
    image->maximum_index = get_u64(bytes + 16);
    image->page_count = get_u64(bytes + 24);
    directory_offset = get_u64(bytes + 32);
    image->data_offset = get_u64(bytes + 40);
    image->packed_bytes = get_u64(bytes + 48);
    uint32_t dictionary_size = bytes[8] == WDL_FORMAT_VERSION ? get_u32(bytes + 56) : 0;
    if (bytes[8] == WDL_FORMAT_VERSION &&
        (!dictionary_size || dictionary_size > WDL_MAX_DICTIONARY_BYTES)) {
        fail("invalid compressed WDL dictionary size"); goto failure;
    }
    if (image->maximum_index == UINT64_MAX) {
        fail("invalid compressed WDL maximum index");
        goto failure;
    }
    calculated_pages = image->packed_bytes / WDL_PAGE_SIZE +
                       (image->packed_bytes % WDL_PAGE_SIZE != 0);
    if (image->packed_bytes != (image->maximum_index + 1) / 2 +
                                   ((image->maximum_index + 1) % 2 != 0) ||
        calculated_pages != image->page_count ||
        directory_offset != WDL_HEADER_SIZE ||
        image->page_count > (UINT64_MAX - WDL_HEADER_SIZE - dictionary_size) /
                                WDL_DIRECTORY_ENTRY_SIZE ||
        image->data_offset != WDL_HEADER_SIZE +
                                  image->page_count * WDL_DIRECTORY_ENTRY_SIZE + dictionary_size ||
        image->data_offset > size) {
        fail("inconsistent compressed WDL header");
        goto failure;
    }
    if (dictionary_size && !prepare_dictionary(
            bytes + image->data_offset - dictionary_size, dictionary_size,
            get_u32(bytes + 60), &image->dictionary)) goto failure;
    for (uint64_t page = 0; page < image->page_count; ++page) {
        const unsigned char *entry =
            bytes + WDL_HEADER_SIZE + page * WDL_DIRECTORY_ENTRY_SIZE;
        uint64_t offset = get_u64(entry);
        uint16_t length = get_u16(entry + 8);
        uint32_t checksum = get_u32(entry + 10);
        if (offset == 0) {
            if (length != 0 || checksum != 0) {
                fail("invalid implicit compressed WDL page %" PRIu64, page);
                goto failure;
            }
        } else if (length > ZSTD_compressBound(WDL_PAGE_SIZE) ||
                   offset < image->data_offset || length == 0 ||
                   offset > size || length > size - offset) {
            fail("invalid compressed WDL page %" PRIu64, page);
            goto failure;
        }
    }
    if (crc32c("123456789", 9) != UINT32_C(0xe3069283)) {
        fail("internal WDL CRC32C self-test failed");
        goto failure;
    }
    *out = image;
    return true;
failure:
    wdl_image_destroy(image);
    return false;
}

void wdl_image_destroy(WdlImage *image)
{
    if (image) ZSTD_freeDDict(image->dictionary);
    free(image);
}

const ZSTD_DDict *wdl_image_dictionary(const WdlImage *image)
{
    return image == NULL ? NULL : image->dictionary;
}

uint64_t wdl_image_maximum_index(const WdlImage *image)
{
    return image == NULL ? 0 : image->maximum_index;
}

uint64_t wdl_image_page_count(const WdlImage *image)
{
    return image == NULL ? 0 : image->page_count;
}

bool wdl_image_page(const WdlImage *image, uint64_t page,
                    const void **compressed, size_t *compressed_size,
                    uint32_t *checksum)
{
    const unsigned char *entry;
    uint64_t offset;
    if (image == NULL || compressed == NULL || compressed_size == NULL ||
        checksum == NULL || page >= image->page_count)
        return fail("invalid compressed WDL page lookup");
    entry = image->data + WDL_HEADER_SIZE +
            page * WDL_DIRECTORY_ENTRY_SIZE;
    offset = get_u64(entry);
    *compressed_size = get_u16(entry + 8);
    *checksum = get_u32(entry + 10);
    *compressed = offset == 0 ? NULL : image->data + offset;
    return true;
}

bool wdl_image_validate_page(const WdlImage *image, uint64_t page,
                             const void *uncompressed, size_t size)
{
    const unsigned char *entry;
    if (image == NULL || uncompressed == NULL ||
        page >= image->page_count || size != WDL_PAGE_SIZE)
        return fail("invalid uncompressed WDL page");
    entry = image->data + WDL_HEADER_SIZE +
            page * WDL_DIRECTORY_ENTRY_SIZE;
    if (get_u64(entry) == 0)
        return true;
    if (crc32c(uncompressed, size) != get_u32(entry + 10))
        return fail("CRC32C mismatch for compressed WDL page %" PRIu64,
                    page);
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
