#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "endgame_index.h"
#include "frontier.h"
#include "generator.h"
#include "gwdegtb.h"
#include "material.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define CHECK(test) do { if (!(test)) { \
    fprintf(stderr, "eight-piece test failed at line %d: %s\n", __LINE__, #test); \
    exit(EXIT_FAILURE); } } while (0)

static uint64_t random_state = UINT64_C(0x926571913284adfe);
static uint64_t random_index(uint64_t count)
{
    random_state ^= random_state >> 12;
    random_state ^= random_state << 25;
    random_state ^= random_state >> 27;
    return (random_state * UINT64_C(2685821657736338717)) % count;
}

static uint64_t choose(unsigned n, unsigned k)
{
    uint64_t result = 1;
    if (k > n)
        return 0;
    for (unsigned i = 1; i <= k; ++i)
        result = result * (n - k + i) / i;
    return result;
}

/* Independent count: choose white men in the bottom row and middle forty,
 * then black men from their remaining 45 squares, then the two king sets. */
static uint64_t expected_count(unsigned wm, unsigned bm, unsigned wk, unsigned bk)
{
    uint64_t men = 0;
    for (unsigned bottom = 0; bottom <= wm && bottom <= 5; ++bottom)
        men += choose(5, bottom) * choose(40, wm - bottom) *
               choose(45 - (wm - bottom), bm);
    return men * choose(50 - wm - bm, wk) * choose(50 - wm - bm - wk, bk);
}

static bool same(const EgPosition *a, const EgPosition *b)
{
    return a->white_men == b->white_men && a->black_men == b->black_men &&
           a->white_kings == b->white_kings && a->black_kings == b->black_kings;
}

static void round_trip(const EgIndexer *indexer, uint64_t index)
{
    EgPosition position;
    uint64_t rank;
    CHECK(eg_index_to_position(indexer, index, &position));
    CHECK(eg_indexer_contains_position(indexer, &position));
    CHECK(eg_position_to_index(indexer, &position, &rank));
    CHECK(rank == index);
}

static uint64_t test_material(unsigned wm, unsigned bm, unsigned wk, unsigned bk)
{
    EgIndexer full;
    CHECK(eg_indexer_init(&full, wm, bm, wk, bk));
    uint64_t count = eg_position_count(&full), sliced_count = 0;
    CHECK(count == expected_count(wm, bm, wk, bk));
    round_trip(&full, 0);
    round_trip(&full, count - 1);
    if (count > UINT64_C(4294967296)) {
        round_trip(&full, UINT64_C(4294967295));
        round_trip(&full, UINT64_C(4294967296));
    }
    for (unsigned i = 0; i < 1000; ++i)
        round_trip(&full, random_index(count));

    for (int wr = wm ? 1 : -1; wr <= (wm ? 9 : -1); ++wr)
        for (int br = bm ? 0 : -1; br <= (bm ? 8 : -1); ++br) {
            EgIndexer slice;
            bool nonempty = (!wm || wm <= (unsigned)(10 - wr) * 5) &&
                            (!bm || bm <= (unsigned)(br + 1) * 5);
            CHECK(eg_slice_indexer_init(&slice, wm, bm, wk, bk, wr, br) == nonempty);
            if (!nonempty)
                continue;
            uint64_t local_count = eg_position_count(&slice);
            sliced_count += local_count;
            for (unsigned sample = 0; sample < 5; ++sample) {
                uint64_t index = sample == 0 ? 0 : sample == 1 ? local_count - 1
                                                               : random_index(local_count);
                EgPosition position, decoded;
                uint64_t global_index;
                int white_row, black_row;
                round_trip(&slice, index);
                CHECK(eg_index_to_position(&slice, index, &position));
                eg_position_slice(&position, &white_row, &black_row);
                CHECK(white_row == wr && black_row == br);
                CHECK(eg_position_to_index(&full, &position, &global_index));
                CHECK(eg_index_to_position(&full, global_index, &decoded));
                CHECK(same(&position, &decoded));
            }
            eg_indexer_destroy(&slice);
        }
    CHECK(sliced_count == count);
    if (wk + wm != 0 && bk + bm != 0) {
        char name[64], mirror[64];
        uint64_t maximum, mirror_maximum;
        size_t bytes, mirror_bytes;
        snprintf(name, sizeof(name), "%uwX-%uwO-%ubX-%ubO", wk, wm, bk, bm);
        snprintf(mirror, sizeof(mirror), "%uwX-%uwO-%ubX-%ubO", bk, bm, wk, wm);
        CHECK(gwdegtb_wdl_info(name, &maximum, &bytes));
        CHECK(gwdegtb_wdl_info(mirror, &mirror_maximum, &mirror_bytes));
        CHECK(maximum == count - 1 && mirror_maximum == maximum);
        CHECK(bytes == (count + 1) / 2 && mirror_bytes == bytes);
        CHECK(!gwdegtb_wdl_is_loaded(wk, wm, bk, bm));
    }
    eg_indexer_destroy(&full);
    return count;
}

static bool read_frontier(uint64_t index, void *context)
{
    uint64_t *expected = context;
    CHECK(index == *expected);
    *expected = 0;
    return true;
}

static uint64_t padded(uint64_t bits)
{
    uint64_t result = 0;
    for (unsigned square = 0; square < 50; ++square)
        if (bits & (UINT64_C(1) << square))
            result |= UINT64_C(1) << (square + 6 + square / 10);
    return result;
}

static void test_gwd_lookup(void)
{
    uint64_t maximum;
    size_t bytes;
    CHECK(gwdegtb_wdl_info("7wX-0wO-1bX-0bO", &maximum, &bytes));
    /* Reserve virtual address space, touching only the queried pages: no
     * multi-GB physical RAM allocation or full WDL generation is needed. */
    unsigned char *data = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(data != MAP_FAILED);
    CHECK(gwdegtb_wdl_attach("1wX-0wO-7bX-0bO", data, bytes));
    CHECK(gwdegtb_wdl_is_loaded(7, 0, 1, 0));
    CHECK(gwdegtb_wdl_is_loaded(1, 0, 7, 0));
    EgIndexer indexer;
    CHECK(eg_indexer_init(&indexer, 0, 0, 7, 1));
    const uint64_t indices[] = {0, 1, UINT64_C(4294967296), maximum};
    for (unsigned i = 0; i < sizeof(indices) / sizeof(indices[0]); ++i) {
        uint64_t index = indices[i];
        CHECK(index <= maximum);
        unsigned shift = (unsigned)(index % 2) * 4;
        data[index / 2] = (unsigned char)((data[index / 2] & ~(15u << shift)) |
                                          (9u << shift)); /* WTM win, BTM loss */
        EgPosition position;
        CHECK(eg_index_to_position(&indexer, index, &position));
        DraughtsPosition source = {position.white_men, position.black_men,
                                   position.white_kings, position.black_kings};
        DraughtsPosition mirror;
        egtb_mirror_position(&source, &mirror);
        CHECK(gwdegtb_wdl_lookup(padded(source.white_kings), 0,
                                  padded(source.black_kings), 0,
                                  GWDEGTB_WHITE_TO_MOVE) == GWDEGTB_WDL_WIN);
        CHECK(gwdegtb_wdl_lookup(padded(source.white_kings), 0,
                                  padded(source.black_kings), 0,
                                  GWDEGTB_BLACK_TO_MOVE) == GWDEGTB_WDL_LOSS);
        CHECK(gwdegtb_wdl_lookup(padded(mirror.white_kings), 0,
                                  padded(mirror.black_kings), 0,
                                  GWDEGTB_BLACK_TO_MOVE) == GWDEGTB_WDL_WIN);
    }
    gwdegtb_wdl_unload_all();
    CHECK(munmap(data, bytes) == 0);
    eg_indexer_destroy(&indexer);
}

/* Real eight-piece indexing/generation on a 25-position corner slice,
 * with synthetic external -300-ply dependencies. Not a solved full EGTB. */
static bool external_loss(const DraughtsPosition *position, EgtbSide side,
                           void *context, int16_t *value)
{
    (void)position;
    (void)side;
    (void)context;
    *value = -300;
    return true;
}

static void test_slice_generation(void)
{
    EgIndexer indexer;
    CHECK(eg_slice_indexer_init(&indexer, 4, 4, 0, 0, 9, 0));
    CHECK(eg_position_count(&indexer) == 25);
    char path[] = "/tmp/gwdegtb-eight-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    close(fd);
    unlink(path);
    Egtb *database = NULL;
    EgtbCreateOptions create = {8, 20, 1};
    EgtbThreadOptions threads = {2, 8, NULL, NULL, 4096};
    EgtbGenerationStatistics stats;
    CHECK(egtb_create(&database, path, 24, 128, &create));
    CHECK(egtb_generate_threaded(database, &indexer, external_loss, NULL,
                                 NULL, NULL, &threads, &stats));
    CHECK(stats.maximum_dtm == 301);
    CHECK(egtb_close(database));
    CHECK(egtb_compact(path, 1, 2));
    CHECK(egtb_open_readonly(&database, path, 2));
    EgtbVerificationOptions verification = {2, 8, NULL, NULL};
    CHECK(egtb_verify_consistent_threaded(database, &indexer, external_loss,
                                          NULL, &verification, NULL, NULL));
    for (uint64_t index = 0; index < 25; ++index)
        for (unsigned side = 0; side < 2; ++side) {
            int16_t value;
            CHECK(egtb_get(database, index, (EgtbSide)side, &value));
            CHECK(value == 301);
        }
    CHECK(egtb_close(database));
    CHECK(unlink(path) == 0);
    eg_indexer_destroy(&indexer);
}

int main(void)
{
    unsigned distributions = 0;
    uint64_t largest = 0;
    for (unsigned wm = 0; wm <= 8; ++wm)
        for (unsigned bm = 0; bm + wm <= 8; ++bm)
            for (unsigned wk = 0; wk + bm + wm <= 8; ++wk) {
                uint64_t count = test_material(wm, bm, wk, 8 - wm - bm - wk);
                if (count > largest)
                    largest = count;
                ++distributions;
            }
    CHECK(distributions == 165);
    EgtbMaterial invalid = {8, 0, 1, 0}, canonical;
    CHECK(egtb_material_resolve(&invalid, &canonical) == EGTB_MATERIAL_INVALID);
    uint64_t maximum;
    size_t bytes;
    CHECK(!gwdegtb_wdl_info("8wX-0wO-1bX-0bO", &maximum, &bytes));
    FrontierStore *frontiers;
    CHECK(frontier_store_create(&frontiers, 2, 1));
    CHECK(frontier_store_append(frontiers, 1, EGTB_BLACK_TO_MOVE, -300, largest - 1));
    CHECK(frontier_store_finish(frontiers));
    uint64_t expected = largest - 1;
    CHECK(frontier_store_visit(frontiers, 1, EGTB_BLACK_TO_MOVE, -300,
                                read_frontier, &expected));
    CHECK(expected == 0);
    frontier_store_destroy(frontiers);
    test_gwd_lookup();
    test_slice_generation();
    printf("eight-piece tests: PASS (%u distributions, slice partitions, GWD sizes, "
           "64-bit frontier, synthetic DTM 301); largest=%" PRIu64 "\n",
           distributions, largest);
    return EXIT_SUCCESS;
}
