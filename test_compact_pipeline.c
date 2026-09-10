#define _POSIX_C_SOURCE 200809L
#include "generator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s: %s / %s\n", __FILE__, __LINE__, #x, egtb_last_error(), egtb_generator_last_error()); exit(1); } } while (0)

static void compare_examples(const EgtbDtmExamples *a, const EgtbDtmExamples *b)
{
    for (unsigned s = 0; s < 2; ++s) {
        REQUIRE(a->longest_win[s].available == b->longest_win[s].available);
        REQUIRE(a->longest_win[s].index == b->longest_win[s].index);
        REQUIRE(a->longest_win[s].dtm == b->longest_win[s].dtm);
        REQUIRE(a->longest_loss[s].available == b->longest_loss[s].available);
        REQUIRE(a->longest_loss[s].index == b->longest_loss[s].index);
        REQUIRE(a->longest_loss[s].dtm == b->longest_loss[s].dtm);
    }
    REQUIRE(a->draw_side == b->draw_side);
    REQUIRE(a->draw.index == b->draw.index && a->draw.dtm == b->draw.dtm);
}

static void check_summary(Egtb *db, const uint64_t *histogram, const EgtbDtmExamples *examples)
{
    uint64_t *expected = calloc(2 * 65536, sizeof(*expected));
    REQUIRE(expected != NULL);
    for (uint64_t i = 0; i <= egtb_maximum_index(db); ++i)
        for (unsigned s = 0; s < 2; ++s) {
            int16_t v; REQUIRE(egtb_get(db, i, (EgtbSide)s, &v));
            ++expected[s * 65536 + (uint16_t)v];
        }
    REQUIRE(memcmp(expected, histogram, 2 * 65536 * sizeof(*expected)) == 0);
    EgtbDtmExamples e = {0};
    REQUIRE(egtb_find_dtm_examples(db, NULL, &e));
    compare_examples(examples, &e);
    EgtbStorageStatistics storage;
    REQUIRE(egtb_storage_statistics(db, &storage));
    uint64_t directory_pages = 2 * egtb_page_count(db);
    REQUIRE(storage.file_bytes == 64 + 10 * directory_pages + storage.live_block_bytes);
    free(expected);
}

/* Terminal captures are the only external successors of king-v-king. */
static bool terminal(const DraughtsPosition *p, EgtbSide side, void *ctx, int16_t *v)
{
    (void)ctx;
    uint64_t pieces = side == EGTB_WHITE_TO_MOVE ? p->white_men | p->white_kings : p->black_men | p->black_kings;
    if (pieces != 0) return false;
    *v = 0; return true;
}

static void pipeline(const char *directory, unsigned threads, bool resident_mode,
                     uint32_t page_size)
{
    char path[512], published[512];
    snprintf(path, sizeof(path), "%s/pipeline-%u-%u.incomplete", directory, threads, resident_mode);
    snprintf(published, sizeof(published), "%s/pipeline-%u-%u.dtm", directory, threads, resident_mode);
    EgIndexer idx = {0}; Egtb *db = NULL; EgtbResident *resident = NULL;
    EgtbCreateOptions create = {4, 20, 1};
    /* One assembly page per worker forces repeated frontier replay. */
    EgtbThreadOptions genopt = {threads, 16, NULL, NULL, 2 * page_size * threads};
    EgtbVerificationOptions verifyopt = {threads, 16, NULL, NULL};
    EgtbGenerationStatistics generated;
    EgtbConsistencyStatistics verification, repair;
    EgtbDtmExamples examples;
    uint64_t *histogram = calloc(2 * 65536, sizeof(*histogram)); REQUIRE(histogram);
    REQUIRE(eg_indexer_init(&idx, 0, 0, 1, 1));
    REQUIRE(egtb_create(&db, path, eg_max_index(&idx), page_size, &create));
    REQUIRE(egtb_compile_threaded(db, &idx, terminal, NULL, &genopt, &generated));
    REQUIRE(generated.consistency_passes == 0);
    REQUIRE(access(published, F_OK) != 0);
    REQUIRE(egtb_finish_compiled(&db, path, &idx, terminal, NULL, &verifyopt,
        resident_mode ? 1048576 : 0, &resident, &verification, &repair, histogram, &examples));
    REQUIRE(repair.passes == 0 && verification.maximum_dtm == 3);
    REQUIRE(verification.positions_checked == 2 * eg_position_count(&idx));
    check_summary(db, histogram, &examples);
    uint64_t damaged_index = examples.longest_win[0].index;
    egtb_resident_destroy(resident); resident = NULL; REQUIRE(egtb_close(db)); db = NULL;
    REQUIRE(egtb_open_readwrite(&db, path, 1));
    REQUIRE(egtb_set(db, damaged_index, EGTB_WHITE_TO_MOVE, 5));
    REQUIRE(egtb_finish_compiled(&db, path, &idx, terminal, NULL, &verifyopt,
        resident_mode ? 1048576 : 0, &resident, &verification, &repair, histogram, &examples));
    REQUIRE(repair.passes > 0 && repair.updates[0] > 0);
    REQUIRE(verification.maximum_dtm == 3 && histogram[5] == 0);
    check_summary(db, histogram, &examples);
    egtb_resident_destroy(resident); resident = NULL; REQUIRE(egtb_close(db)); db = NULL;
    /* Break a live block checksum. This must never enter the repair path. */
    int fd = open(path, O_RDWR); REQUIRE(fd >= 0);
    unsigned char offset_bytes[8]; uint64_t offset = 0;
    REQUIRE(pread(fd, offset_bytes, 8, 64) == 8);
    for (unsigned i = 0; i < 8; ++i) offset |= (uint64_t)offset_bytes[i] << (8 * i);
    REQUIRE(offset != 0);
    unsigned char checksum; REQUIRE(pread(fd, &checksum, 1, (off_t)offset) == 1);
    unsigned char changed = checksum ^ 1; REQUIRE(pwrite(fd, &changed, 1, (off_t)offset) == 1); REQUIRE(close(fd) == 0);
    REQUIRE(egtb_open_readwrite(&db, path, 1));
    REQUIRE(!egtb_finish_compiled(&db, path, &idx, terminal, NULL, &verifyopt,
        resident_mode ? 1048576 : 0, &resident, &verification, &repair, histogram, &examples));
    REQUIRE(repair.passes == 0 && access(published, F_OK) != 0);
    egtb_resident_destroy(resident); REQUIRE(egtb_close(db)); db = NULL;
    fd = open(path, O_RDWR); REQUIRE(fd >= 0);
    REQUIRE(pwrite(fd, &checksum, 1, (off_t)offset) == 1); REQUIRE(close(fd) == 0);
    REQUIRE(egtb_publish(path, published));
    REQUIRE(access(path, F_OK) != 0 && access(published, F_OK) == 0);
    REQUIRE(unlink(published) == 0);
    eg_indexer_destroy(&idx); free(histogram);
}

/* Incompressible valid codes exercise full batches, partial final pages,
 * draw-only pages, and the wide-value escape encoding. */
static void batches(const char *directory)
{
    char path[512]; snprintf(path, sizeof(path), "%s/batches.dtm", directory);
    const uint64_t n = 2500003;
    EgtbCreateOptions create = {1, 20, 1}; Egtb *db = NULL;
    EgtbPageWriter *writer = NULL; EgtbEntry entries[1024];
    REQUIRE(egtb_create(&db, path, n - 1, 2048, &create));
    REQUIRE(egtb_prepare_compact(db));
    REQUIRE(egtb_page_writer_create(&writer, db, 0, egtb_page_count(db)));
    uint32_t rng = 123;
    for (uint64_t i = 0; i < n;) {
        size_t count = n - i < 1024 ? (size_t)(n - i) : 1024;
        for (size_t j = 0; j < count; ++j) {
            rng = rng * 1664525u + 1013904223u;
            entries[j].white_to_move = (int16_t)((rng >> 16) % 32767 - 16383);
            entries[j].black_to_move = EGTB_STORED_DRAW;
        }
        REQUIRE(egtb_page_writer_put(writer, entries, count)); i += count;
    }
    REQUIRE(egtb_page_writer_close(writer)); REQUIRE(egtb_close(db));
    REQUIRE(egtb_open_readonly(&db, path, 1)); rng = 123;
    for (uint64_t i = 0; i < n; ++i) {
        rng = rng * 1664525u + 1013904223u;
        int16_t code = (int16_t)((rng >> 16) % 32767 - 16383), v;
        REQUIRE(egtb_get(db, i, EGTB_WHITE_TO_MOVE, &v)); REQUIRE(v == egtb_decode_dtm(code));
        REQUIRE(egtb_get(db, i, EGTB_BLACK_TO_MOVE, &v)); REQUIRE(v == EGTB_DRAW);
    }
    EgtbStorageStatistics storage; REQUIRE(egtb_storage_statistics(db, &storage));
    REQUIRE(storage.live_block_bytes > 4 * 1024 * 1024);
    REQUIRE(storage.file_bytes == 64 + 20 * egtb_page_count(db) + storage.live_block_bytes);
    REQUIRE(egtb_close(db)); REQUIRE(unlink(path) == 0);
}

int main(void)
{
    char directory[] = "/tmp/gwdegtb-pipeline-test-XXXXXX";
    REQUIRE(mkdtemp(directory));
    pipeline(directory, 1, false, 2048); pipeline(directory, 4, true, 2048);
    pipeline(directory, 16, false, 128);
    batches(directory);
    REQUIRE(rmdir(directory) == 0);
    puts("compact pipeline: verification summaries, repair fallback, corruption rejection and batches PASS");
    return 0;
}
