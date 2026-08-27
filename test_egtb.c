#define _POSIX_C_SOURCE 200809L

#include "egtb.h"
#include "endgame_index.h"
#include "wdl.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    uint64_t wins[2];
    uint64_t losses[2];
    uint64_t draws[2];
} Statistics;

static uint64_t random_state = UINT64_C(0xd1b54a32d192ed03);

static uint64_t next_random(void)
{
    uint64_t value = random_state;
    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    random_state = value;
    return value * UINT64_C(2685821657736338717);
}

static uint64_t read_little_u64(const unsigned char bytes[8])
{
    uint64_t value = 0;
    unsigned i;
    for (i = 0; i < 8; ++i)
        value |= (uint64_t)bytes[i] << (8 * i);
    return value;
}

static bool collect_statistics(Egtb *egtb, Statistics *statistics)
{
    uint64_t index;
    memset(statistics, 0, sizeof(*statistics));
    for (index = 0; index <= egtb_maximum_index(egtb); ++index) {
        unsigned side;
        for (side = 0; side < 2; ++side) {
            int16_t value;
            if (!egtb_get(egtb, index, (EgtbSide)side, &value))
                return false;
            if (value == EGTB_DRAW)
                ++statistics->draws[side];
            else if (value > 0)
                ++statistics->wins[side];
            else
                ++statistics->losses[side];
        }
    }
    return true;
}

static bool same_statistics(const Statistics *a, const Statistics *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

static bool test_direct_cache_views(void)
{
    char handle_path[] = "/tmp/ipd-egtb-handle-XXXXXX";
    char direct_path[] = "/tmp/ipd-egtb-direct-XXXXXX";
    EgtbCreateOptions options = {4, 20, 3};
    Egtb *handle = NULL, *direct = NULL;
    EgtbView *write_view = NULL, *read_view = NULL;
    EgtbCacheStatistics cache_statistics;
    uint64_t state = UINT64_C(0x4d595df4d0f33173);
    const uint64_t positions = 16384;
    unsigned write_number;
    int handle_descriptor = mkstemp(handle_path);
    int direct_descriptor = mkstemp(direct_path);
    bool ok = false;
    if (handle_descriptor < 0 || direct_descriptor < 0)
        goto done;
    close(handle_descriptor);
    close(direct_descriptor);
    handle_descriptor = direct_descriptor = -1;
    unlink(handle_path);
    unlink(direct_path);
    if (!egtb_create(&handle, handle_path, positions - 1, 1024, &options) ||
        !egtb_create(&direct, direct_path, positions - 1, 1024, &options) ||
        !egtb_view_create(&write_view, direct, 4, true))
        goto done;
    for (write_number = 0; write_number < 32768; ++write_number) {
        uint64_t index;
        EgtbSide side;
        int16_t value, read_back;
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        index = (state * UINT64_C(2685821657736338717)) % positions;
        side = (EgtbSide)(state & 1);
        value = (state & 2) != 0
                    ? (int16_t)(1 + 2 * (state % 256))
                    : (int16_t)(-2 * (int)(state % 257));
        if (!egtb_set(handle, index, side, value) ||
            !egtb_view_set(write_view, index, side, value) ||
            !egtb_view_get(write_view, index, side, &read_back) ||
            read_back != value)
            goto done;
    }
    egtb_view_cache_statistics(write_view, &cache_statistics);
    if (cache_statistics.lookups == 0 || cache_statistics.hits == 0 ||
        cache_statistics.misses == 0 ||
        cache_statistics.dirty_evictions == 0)
        goto done;
    if (!egtb_view_close(write_view))
        goto done;
    write_view = NULL;
    if (!egtb_flush(handle))
        goto done;
    for (uint64_t index = 0; index < positions; ++index) {
        for (unsigned side = 0; side < 2; ++side) {
            int16_t handle_value, direct_value;
            if (!egtb_get(handle, index, (EgtbSide)side, &handle_value) ||
                !egtb_get(direct, index, (EgtbSide)side, &direct_value) ||
                handle_value != direct_value)
                goto done;
        }
    }
    if (!egtb_close(handle) || !egtb_close(direct))
        goto done;
    handle = direct = NULL;
    if (!egtb_open_readonly(&handle, handle_path, 4) ||
        !egtb_open_readonly(&direct, direct_path, 1) ||
        !egtb_view_create(&read_view, direct, 4, false))
        goto done;
    for (uint64_t index = 0; index < positions; ++index) {
        for (unsigned side = 0; side < 2; ++side) {
            int16_t handle_value, direct_value;
            if (!egtb_get(handle, index, (EgtbSide)side, &handle_value) ||
                !egtb_view_get(read_view, index, (EgtbSide)side,
                               &direct_value) ||
                handle_value != direct_value)
                goto done;
        }
    }
    egtb_view_cache_statistics(read_view, &cache_statistics);
    if (cache_statistics.lookups != positions * 2 ||
        cache_statistics.hits == 0 || cache_statistics.misses == 0)
        goto done;
    ok = true;
done:
    if (write_view != NULL)
        egtb_view_close(write_view);
    if (read_view != NULL)
        egtb_view_close(read_view);
    if (handle != NULL)
        egtb_close(handle);
    if (direct != NULL)
        egtb_close(direct);
    if (handle_descriptor >= 0)
        close(handle_descriptor);
    if (direct_descriptor >= 0)
        close(direct_descriptor);
    unlink(handle_path);
    unlink(direct_path);
    return ok;
}

typedef struct {
    EgtbView *view;
    uint64_t first;
    uint64_t end;
    bool failed;
} RangedWriter;

static void *write_ranged_view(void *opaque)
{
    RangedWriter *writer = opaque;
    for (uint64_t index = writer->first; index < writer->end; ++index) {
        int16_t white = (int16_t)(1 + 2 * (index % 32));
        int16_t black = (int16_t)(-2 * (int)(index % 33));
        if (!egtb_view_set(writer->view, index, EGTB_WHITE_TO_MOVE, white) ||
            !egtb_view_set(writer->view, index, EGTB_BLACK_TO_MOVE, black)) {
            writer->failed = true;
            break;
        }
    }
    for (uint64_t index = writer->first;
         !writer->failed && index < writer->end; ++index) {
        int16_t white, black;
        if (!egtb_view_get(writer->view, index, EGTB_WHITE_TO_MOVE, &white) ||
            !egtb_view_get(writer->view, index, EGTB_BLACK_TO_MOVE, &black) ||
            white != (int16_t)(1 + 2 * (index % 32)) ||
            black != (int16_t)(-2 * (int)(index % 33)))
            writer->failed = true;
    }
    return NULL;
}

static bool test_concurrent_ranged_views(void)
{
    char path[] = "/tmp/ipd-egtb-ranged-XXXXXX";
    EgtbCreateOptions options = {4, 20, 3};
    Egtb *database = NULL;
    EgtbView *views[2] = {NULL, NULL};
    RangedWriter writers[2] = {0};
    pthread_t threads[2];
    const uint64_t positions = 64 * 256;
    int descriptor = mkstemp(path);
    unsigned created = 0;
    bool ok = false;
    if (descriptor < 0)
        return false;
    close(descriptor);
    unlink(path);
    if (!egtb_create(&database, path, positions - 1, 1024, &options) ||
        !egtb_view_create_range(&views[0], database, 1, true, 0, 32) ||
        !egtb_view_create_range(&views[1], database, 1, true, 32, 64))
        goto done;
    writers[0] = (RangedWriter){views[0], 0, positions / 2, false};
    writers[1] = (RangedWriter){views[1], positions / 2, positions, false};
    for (unsigned i = 0; i < 2; ++i) {
        if (pthread_create(&threads[i], NULL, write_ranged_view,
                           &writers[i]) != 0)
            goto join;
        ++created;
    }
join:
    for (unsigned i = 0; i < created; ++i)
        if (pthread_join(threads[i], NULL) != 0)
            goto done;
    if (created != 2 || writers[0].failed || writers[1].failed)
        goto done;
    for (unsigned i = 0; i < 2; ++i) {
        if (!egtb_view_close(views[i]))
            goto done;
        views[i] = NULL;
    }
    for (uint64_t index = 0; index < positions; ++index) {
        int16_t white, black;
        if (!egtb_get(database, index, EGTB_WHITE_TO_MOVE, &white) ||
            !egtb_get(database, index, EGTB_BLACK_TO_MOVE, &black) ||
            white != (int16_t)(1 + 2 * (index % 32)) ||
            black != (int16_t)(-2 * (int)(index % 33)))
            goto done;
    }
    ok = true;
done:
    for (unsigned i = 0; i < 2; ++i)
        if (views[i] != NULL)
            egtb_view_close(views[i]);
    if (database != NULL)
        egtb_close(database);
    unlink(path);
    return ok;
}

static bool collect_wdl_statistics(Wdl *wdl, WdlStatistics *statistics)
{
    uint64_t index;
    memset(statistics, 0, sizeof(*statistics));
    for (index = 0; index <= wdl_maximum_index(wdl); ++index) {
        unsigned side;
        for (side = 0; side < 2; ++side) {
            WdlResult result;
            if (!wdl_get(wdl, index, (EgtbSide)side, &result))
                return false;
            if (result == WDL_WIN)
                ++statistics->wins[side];
            else if (result == WDL_LOSS)
                ++statistics->losses[side];
            else
                ++statistics->draws[side];
        }
    }
    return true;
}

static bool statistics_match_wdl(const Statistics *dtm,
                                 const WdlStatistics *wdl)
{
    unsigned side;
    for (side = 0; side < 2; ++side) {
        if (dtm->wins[side] != wdl->wins[side] ||
            dtm->losses[side] != wdl->losses[side] ||
            dtm->draws[side] != wdl->draws[side])
            return false;
    }
    return true;
}

static void print_statistics(const Statistics *statistics)
{
    printf("WTM: wins=%" PRIu64 " losses=%" PRIu64 " draws=%" PRIu64 "\n",
           statistics->wins[0], statistics->losses[0], statistics->draws[0]);
    printf("BTM: wins=%" PRIu64 " losses=%" PRIu64 " draws=%" PRIu64 "\n",
           statistics->wins[1], statistics->losses[1], statistics->draws[1]);
}

static void print_storage_statistics(const char *label,
                                     const EgtbStorageStatistics *statistics)
{
    double payload_percent = 100.0 * (double)statistics->compressed_payload_bytes /
                             (double)statistics->logical_uncompressed_bytes;
    double overall_percent = 100.0 * (double)statistics->file_bytes /
                             (double)statistics->logical_uncompressed_bytes;
    printf("%s: payload=%.2f%% overall=%.2f%% (%.2f:1), "
           "live-pages=%" PRIu64 " file=%" PRIu64 " bytes\n",
           label, payload_percent, overall_percent,
           (double)statistics->logical_uncompressed_bytes /
               (double)statistics->file_bytes,
           statistics->live_pages, statistics->file_bytes);
}

static int report_failure(const char *operation, const char *path)
{
    fprintf(stderr, "%s failed: %s\n", operation, egtb_last_error());
    if (path != NULL)
        unlink(path);
    return EXIT_FAILURE;
}

int main(void)
{
    char stem[] = "/tmp/ipd-egtb-regression-XXXXXX";
    char path[sizeof(stem) + 4];
    char wdl_path[sizeof(stem) + 4];
    char material_name[64];
    EgtbCreateOptions options = {256, 20, 3};
    EgIndexer indexer;
    Egtb *egtb = NULL, *shared = NULL;
    Wdl *wdl = NULL;
    Statistics before, after;
    WdlStatistics wdl_statistics;
    WdlStorageStatistics wdl_storage;
    EgtbStorageStatistics before_storage, after_storage;
    uint64_t count, writes, write_number, last_index = 0;
    struct stat before_compaction, after_compaction;
    int descriptor;

    if (!test_direct_cache_views()) {
        fprintf(stderr, "direct-mapped cache-view regression failed: %s\n",
                egtb_last_error());
        return EXIT_FAILURE;
    }
    if (!test_concurrent_ranged_views()) {
        fprintf(stderr, "concurrent ranged-view regression failed: %s\n",
                egtb_last_error());
        return EXIT_FAILURE;
    }

    descriptor = mkstemp(stem);
    if (descriptor < 0) {
        perror("mkstemp");
        return EXIT_FAILURE;
    }
    close(descriptor);
    unlink(stem);
    snprintf(path, sizeof(path), "%s.dtm", stem);
    snprintf(wdl_path, sizeof(wdl_path), "%s.wdl", stem);
    if (!egtb_material_filename(material_name, sizeof(material_name),
                                3, 1, 1, 2, "dtm") ||
        strcmp(material_name, "3wX-1wO-1bX-2bO.dtm") != 0) {
        fprintf(stderr, "material filename generation failed\n");
        return EXIT_FAILURE;
    }

    if (!eg_indexer_init(&indexer, 1, 1, 1, 1))
        return report_failure("indexer initialization", path);
    count = eg_position_count(&indexer);
    eg_indexer_destroy(&indexer);

    if (!egtb_create(&egtb, path, count - 1, 1024, &options))
        return report_failure("EGTB creation", path);
    if (egtb_page_size(egtb) != 1024 ||
        egtb_maximum_index(egtb) != count - 1 || egtb_is_readonly(egtb))
        return report_failure("created-header validation", path);

    writes = count / 10;
    for (write_number = 0; write_number < writes; ++write_number) {
        uint64_t index = next_random() % count;
        EgtbSide side = (next_random() & 1) != 0 ? EGTB_WHITE_TO_MOVE :
                                                    EGTB_BLACK_TO_MOVE;
        int16_t value;
        int16_t read_back;
        if ((next_random() & 1) != 0)
            value = (int16_t)(1 + 2 * (next_random() % 256));
        else
            value = (int16_t)(-2 * (int)(next_random() % 257));
        if (!egtb_set(egtb, index, side, value) ||
            !egtb_get(egtb, index, side, &read_back) || read_back != value)
            return report_failure("cached write/read validation", path);
        last_index = index;
    }

    if (!collect_statistics(egtb, &before))
        return report_failure("pre-compaction scan", path);
    if (!egtb_flush(egtb) ||
        !egtb_storage_statistics(egtb, &before_storage))
        return report_failure("pre-compaction storage statistics", path);
    if (!egtb_close(egtb))
        return report_failure("read/write close", path);
    egtb = NULL;
    if (stat(path, &before_compaction) != 0) {
        perror("stat before compaction");
        unlink(path);
        return EXIT_FAILURE;
    }
    if (!egtb_compact(path, 9, 8))
        return report_failure("compaction", path);
    if (stat(path, &after_compaction) != 0) {
        perror("stat after compaction");
        unlink(path);
        return EXIT_FAILURE;
    }
    if (after_compaction.st_size > before_compaction.st_size) {
        fprintf(stderr, "compaction unexpectedly increased the file size\n");
        unlink(path);
        return EXIT_FAILURE;
    }

    if (!egtb_open_readonly(&egtb, path, 8))
        return report_failure("read-only reopen", path);
    if (!egtb_is_readonly(egtb) || egtb_page_size(egtb) != 1024 ||
        egtb_maximum_index(egtb) != count - 1)
        return report_failure("reopened-header validation", path);
    if (!egtb_open_readonly(&shared, path, 64) || shared != egtb)
        return report_failure("shared read-only open", path);
    if (!egtb_close(shared))
        return report_failure("shared handle release", path);
    shared = NULL;
    if (!collect_statistics(egtb, &after))
        return report_failure("post-compaction scan", path);
    if (!same_statistics(&before, &after)) {
        fprintf(stderr, "statistics changed after close/compact/reopen\n");
        print_statistics(&before);
        print_statistics(&after);
        egtb_close(egtb);
        unlink(path);
        return EXIT_FAILURE;
    }
    if (!egtb_storage_statistics(egtb, &after_storage))
        return report_failure("post-compaction storage statistics", path);
    if (!egtb_close(egtb))
        return report_failure("read-only close", path);
    egtb = NULL;

    if (access(wdl_path, F_OK) == 0) {
        fprintf(stderr, "WDL test target unexpectedly exists\n");
        unlink(path);
        unlink(wdl_path);
        return EXIT_FAILURE;
    }
    if (!wdl_open(&wdl, wdl_path, 8, 9, 8)) {
        fprintf(stderr, "WDL generation/open failed: %s\n", wdl_last_error());
        unlink(path);
        unlink(wdl_path);
        return EXIT_FAILURE;
    }
    if (wdl_page_size(wdl) != WDL_PAGE_SIZE ||
        wdl_maximum_index(wdl) != count - 1 ||
        !collect_wdl_statistics(wdl, &wdl_statistics) ||
        !statistics_match_wdl(&after, &wdl_statistics) ||
        !wdl_storage_statistics(wdl, &wdl_storage)) {
        fprintf(stderr, "WDL read-back/statistics validation failed: %s\n",
                wdl_last_error());
        wdl_close(wdl);
        unlink(path);
        unlink(wdl_path);
        return EXIT_FAILURE;
    }
    if (!wdl_close(wdl)) {
        wdl = NULL;
        fprintf(stderr, "WDL close failed: %s\n", wdl_last_error());
        unlink(path);
        unlink(wdl_path);
        return EXIT_FAILURE;
    }
    wdl = NULL;

    {
        unsigned char directory_entry[14];
        unsigned char original_checksum_byte;
        uint64_t page = 0;
        uint64_t page_count = (count + 2047) / 2048;
        uint64_t lookup_index;
        long checksum_offset;
        WdlResult ignored;
        FILE *file = fopen(wdl_path, "r+b");
        bool prepared = file != NULL;
        while (prepared && page < page_count) {
            prepared = fseek(file, (long)(64 + page * 14), SEEK_SET) == 0 &&
                       fread(directory_entry, 1, sizeof(directory_entry), file) ==
                           sizeof(directory_entry);
            if (prepared && read_little_u64(directory_entry) != 0)
                break;
            ++page;
        }
        prepared = prepared && page < page_count;
        lookup_index = page * 2048;
        checksum_offset = (long)(64 + page * 14 + 10);
        prepared = prepared && fseek(file, checksum_offset, SEEK_SET) == 0 &&
                        fread(&original_checksum_byte, 1, 1, file) == 1 &&
                        fseek(file, checksum_offset, SEEK_SET) == 0 &&
                        fputc(original_checksum_byte ^ 0x80, file) != EOF;
        if (file != NULL)
            prepared = fclose(file) == 0 && prepared;
        if (!prepared) {
            fprintf(stderr, "could not prepare WDL checksum test\n");
            unlink(path);
            unlink(wdl_path);
            return EXIT_FAILURE;
        }
        if (!wdl_open(&wdl, wdl_path, 1, 9, 1) ||
            wdl_get(wdl, lookup_index, EGTB_WHITE_TO_MOVE, &ignored) ||
            strstr(wdl_last_error(), "CRC32C mismatch") == NULL) {
            fprintf(stderr, "WDL checksum corruption was not detected: %s\n",
                    wdl_last_error());
            wdl_close(wdl);
            unlink(path);
            unlink(wdl_path);
            return EXIT_FAILURE;
        }
        if (!wdl_close(wdl)) {
            wdl = NULL;
            fprintf(stderr, "corrupt WDL close failed: %s\n", wdl_last_error());
            unlink(path);
            unlink(wdl_path);
            return EXIT_FAILURE;
        }
        wdl = NULL;
        file = fopen(wdl_path, "r+b");
        prepared = file != NULL && fseek(file, checksum_offset, SEEK_SET) == 0 &&
                   fputc(original_checksum_byte, file) != EOF;
        if (file != NULL)
            prepared = fclose(file) == 0 && prepared;
        if (!prepared || !wdl_open(&wdl, wdl_path, 1, 9, 1) ||
            !wdl_get(wdl, lookup_index, EGTB_WHITE_TO_MOVE, &ignored)) {
            fprintf(stderr, "WDL checksum restoration failed: %s\n",
                    wdl_last_error());
            wdl_close(wdl);
            unlink(path);
            unlink(wdl_path);
            return EXIT_FAILURE;
        }
        if (!wdl_close(wdl)) {
            wdl = NULL;
            fprintf(stderr, "restored WDL close failed: %s\n",
                    wdl_last_error());
            unlink(path);
            unlink(wdl_path);
            return EXIT_FAILURE;
        }
        wdl = NULL;
    }

    {
        unsigned char directory_entry[10];
        unsigned char original_checksum_byte;
        uint64_t page = last_index / 256;
        uint64_t block_offset;
        int16_t ignored;
        FILE *file = fopen(path, "r+b");
        bool prepared = file != NULL &&
                        fseek(file, (long)(64 + page * 10), SEEK_SET) == 0 &&
                        fread(directory_entry, 1, sizeof(directory_entry), file) ==
                            sizeof(directory_entry);
        block_offset = prepared ? read_little_u64(directory_entry) : 0;
        prepared = prepared && block_offset != 0 &&
                   fseek(file, (long)block_offset, SEEK_SET) == 0 &&
                   fread(&original_checksum_byte, 1, 1, file) == 1 &&
                   fseek(file, (long)block_offset, SEEK_SET) == 0 &&
                   fputc(original_checksum_byte ^ 0x80, file) != EOF;
        if (file != NULL) {
            prepared = fclose(file) == 0 && prepared;
            file = NULL;
        }
        if (!prepared)
            return report_failure("checksum-corruption preparation", path);
        if (!egtb_open_readonly(&egtb, path, 1))
            return report_failure("checksum-corruption reopen", path);
        if (egtb_get(egtb, last_index, EGTB_WHITE_TO_MOVE, &ignored) ||
            strstr(egtb_last_error(), "CRC32C mismatch") == NULL) {
            fprintf(stderr, "uncompressed-page checksum corruption was not detected\n");
            egtb_close(egtb);
            unlink(path);
            return EXIT_FAILURE;
        }
        if (!egtb_close(egtb))
            return report_failure("checksum-corruption close", path);
        egtb = NULL;
        file = fopen(path, "r+b");
        prepared = file != NULL && fseek(file, (long)block_offset, SEEK_SET) == 0 &&
                   fputc(original_checksum_byte, file) != EOF;
        if (file != NULL) {
            prepared = fclose(file) == 0 && prepared;
            file = NULL;
        }
        if (!prepared)
            return report_failure("checksum restoration", path);
    }

    {
        FILE *file = fopen(path, "r+b");
        bool prepared = file != NULL && fseek(file, 8, SEEK_SET) == 0 &&
                        fputc(EGTB_FORMAT_VERSION + 1, file) != EOF;
        if (file != NULL) {
            prepared = fclose(file) == 0 && prepared;
            file = NULL;
        }
        if (!prepared) {
            fprintf(stderr, "could not prepare version-rejection test\n");
            unlink(path);
            return EXIT_FAILURE;
        }
        if (egtb_open_readonly(&egtb, path, 1)) {
            fprintf(stderr, "unsupported EGTB version was accepted\n");
            egtb_close(egtb);
            unlink(path);
            return EXIT_FAILURE;
        }
    }

    printf("EGTB regression: PASS\n");
    printf("positions=%" PRIu64 " randomized writes=%" PRIu64
           " pages=%" PRIu64 "\n", count, writes,
           (count + 255) / 256);
    print_statistics(&after);
    print_storage_statistics("before compaction", &before_storage);
    print_storage_statistics("after compaction", &after_storage);
    printf("WDL: raw=%" PRIu64 " payload=%" PRIu64 " file=%" PRIu64
           " bytes overall=%.2f%% (%.2f:1)\n",
           wdl_storage.logical_uncompressed_bytes,
           wdl_storage.compressed_payload_bytes, wdl_storage.file_bytes,
           100.0 * (double)wdl_storage.file_bytes /
               (double)wdl_storage.logical_uncompressed_bytes,
           (double)wdl_storage.logical_uncompressed_bytes /
               (double)wdl_storage.file_bytes);
    unlink(wdl_path);
    unlink(path);
    return EXIT_SUCCESS;
}
