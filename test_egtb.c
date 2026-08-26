#define _POSIX_C_SOURCE 200809L

#include "egtb.h"
#include "endgame_index.h"
#include "wdl.h"

#include <inttypes.h>
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
