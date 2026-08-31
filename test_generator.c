#define _POSIX_C_SOURCE 200809L

#include "egtb.h"
#include "endgame_index.h"
#include "generator.h"
#include "movegen.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BIT(square) (UINT64_C(1) << (square))

typedef struct {
    Egtb *database;
    const EgIndexer *indexer;
    DraughtsPosition position;
    size_t moves;
    bool failed;
} SeedSuccessorContext;

typedef struct {
    uint64_t reports;
    bool saw_shorter_win;
    bool saw_longer_loss;
} ConsistencyReportCheck;

typedef struct {
    uint64_t probes;
} ExternalProbeCheck;

static bool probe_lost_in_four(const DraughtsPosition *position,
                               EgtbSide side, void *opaque, int16_t *value)
{
    ExternalProbeCheck *check = opaque;
    (void)position;
    (void)side;
    ++check->probes;
    *value = -4;
    return true;
}

static void check_consistency_report(
    uint64_t index, EgtbSide side, const DraughtsPosition *position,
    int16_t old_value, int16_t new_value, void *opaque)
{
    ConsistencyReportCheck *check = opaque;
    (void)index;
    (void)side;
    (void)position;
    ++check->reports;
    if (old_value > 0 && new_value > 0 && new_value < old_value)
        check->saw_shorter_win = true;
    if (old_value != EGTB_DRAW && old_value <= 0 && new_value < old_value)
        check->saw_longer_loss = true;
}

static bool seed_successor(const DraughtsMove *move, void *opaque)
{
    SeedSuccessorContext *context = opaque;
    DraughtsPosition successor = context->position;
    EgPosition indexed;
    uint64_t index;
    int16_t value = context->moves == 0 ? 3 : 1;
    if (!draughts_do_move(&successor, EGTB_WHITE_TO_MOVE, move, NULL)) {
        context->failed = true;
        return false;
    }
    indexed.white_men = successor.white_men;
    indexed.black_men = successor.black_men;
    indexed.white_kings = successor.white_kings;
    indexed.black_kings = successor.black_kings;
    if (!eg_position_to_index(context->indexer, &indexed, &index) ||
        !egtb_set(context->database, index, EGTB_BLACK_TO_MOVE, value)) {
        context->failed = true;
        return false;
    }
    ++context->moves;
    return true;
}

static bool test_parameterized_win_backtrack(void)
{
    char path[] = "/tmp/ipd-parameterized-backtrack-XXXXXX";
    EgtbCreateOptions options = {16, 20, 3};
    EgtbLossBacktrackStatistics first, shortened;
    SeedSuccessorContext seed;
    EgIndexer indexer;
    EgPosition indexed;
    Egtb *database = NULL;
    uint64_t predecessor_index;
    size_t move_count;
    int16_t value;
    int descriptor = mkstemp(path);
    bool ok = false;
    if (descriptor < 0)
        return false;
    close(descriptor);
    unlink(path);
    if (!eg_indexer_init(&indexer, 0, 0, 1, 1))
        goto done;
    if (!egtb_create(&database, path, eg_max_index(&indexer), 1024, &options))
        goto destroy_indexer;
    seed.database = database;
    seed.indexer = &indexer;
    seed.position = (DraughtsPosition){0};
    seed.position.white_kings = BIT(0);
    seed.position.black_kings = BIT(49);
    seed.moves = 0;
    seed.failed = false;
    if (draughts_has_capture(&seed.position, EGTB_WHITE_TO_MOVE) ||
        !draughts_generate_moves(&seed.position, EGTB_WHITE_TO_MOVE,
                                 seed_successor, &seed, &move_count) ||
        seed.failed || move_count != seed.moves || move_count < 2)
        goto destroy_indexer;
    indexed.white_men = seed.position.white_men;
    indexed.black_men = seed.position.black_men;
    indexed.white_kings = seed.position.white_kings;
    indexed.black_kings = seed.position.black_kings;
    if (!eg_position_to_index(&indexer, &indexed, &predecessor_index) ||
        !egtb_backtrack_wins_to_losses(database, &indexer, 3, &first) ||
        !egtb_get(database, predecessor_index, EGTB_WHITE_TO_MOVE, &value) ||
        value != -4)
        goto destroy_indexer;
    if (!egtb_set(database, predecessor_index, EGTB_WHITE_TO_MOVE, -6) ||
        !egtb_backtrack_wins_to_losses(database, &indexer, 3, &shortened) ||
        !egtb_get(database, predecessor_index, EGTB_WHITE_TO_MOVE, &value) ||
        value != -4 || shortened.shortened_losses[EGTB_WHITE_TO_MOVE] == 0)
        goto destroy_indexer;
    ok = true;
destroy_indexer:
    eg_indexer_destroy(&indexer);
done:
    if (database != NULL && !egtb_close(database))
        ok = false;
    unlink(path);
    return ok;
}

static bool test_external_promotion_initialization(void)
{
    char path[] = "/tmp/ipd-external-init-XXXXXX";
    EgtbCreateOptions options = {16, 20, 3};
    EgtbInitializationStatistics statistics;
    ExternalProbeCheck probe = {0};
    DraughtsPosition position = {0};
    EgPosition indexed;
    EgIndexer indexer;
    Egtb *database = NULL;
    uint64_t index;
    int16_t value;
    int descriptor = mkstemp(path);
    bool ok = false;
    if (descriptor < 0)
        return false;
    close(descriptor);
    unlink(path);
    if (!eg_indexer_init(&indexer, 1, 1, 0, 0))
        goto done;
    if (!egtb_create(&database, path, eg_max_index(&indexer), 1024, &options) ||
        !egtb_initialize_terminal_positions_with_probe(
            database, &indexer, probe_lost_in_four, &probe, &statistics))
        goto destroy_indexer;
    position.white_men = BIT(5);
    position.black_men = BIT(44);
    indexed.white_men = position.white_men;
    indexed.black_men = position.black_men;
    indexed.white_kings = 0;
    indexed.black_kings = 0;
    if (draughts_has_capture(&position, EGTB_WHITE_TO_MOVE) ||
        !eg_position_to_index(&indexer, &indexed, &index) ||
        !egtb_get(database, index, EGTB_WHITE_TO_MOVE, &value) ||
        value != 5 || probe.probes == 0 ||
        statistics.external_wins[EGTB_WHITE_TO_MOVE] == 0)
        goto destroy_indexer;
    ok = true;
destroy_indexer:
    eg_indexer_destroy(&indexer);
done:
    if (database != NULL && !egtb_close(database))
        ok = false;
    unlink(path);
    return ok;
}

static bool test_external_promotion_backtrack(void)
{
    char path[] = "/tmp/ipd-external-backtrack-XXXXXX";
    EgtbCreateOptions options = {16, 20, 3};
    EgtbLossBacktrackStatistics statistics;
    ExternalProbeCheck probe = {0};
    EgIndexer indexer;
    Egtb *database = NULL;
    int descriptor = mkstemp(path);
    bool ok = false;
    if (descriptor < 0)
        return false;
    close(descriptor);
    unlink(path);
    if (!eg_indexer_init(&indexer, 1, 0, 1, 1))
        goto done;
    if (!egtb_create(&database, path, eg_max_index(&indexer), 1024,
                     &options) ||
        !egtb_set(database, 59310, EGTB_BLACK_TO_MOVE, 1) ||
        !egtb_backtrack_wins_to_losses_with_probe(
            database, &indexer, 1, probe_lost_in_four, &probe,
            &statistics) ||
        probe.probes == 0)
        goto destroy_indexer;
    ok = true;
destroy_indexer:
    eg_indexer_destroy(&indexer);
done:
    if (database != NULL && !egtb_close(database))
        ok = false;
    unlink(path);
    return ok;
}

static bool test_complete_wk_bk_generation(void)
{
    char path[] = "/tmp/ipd-complete-generator-XXXXXX";
    EgtbCreateOptions options = {16, 20, 3};
    EgtbGenerationStatistics statistics;
    EgtbConsistencyStatistics consistency;
    Bitmap verified_positions = {0};
    ConsistencyReportCheck report = {0};
    EgIndexer indexer;
    Egtb *database = NULL;
    EgtbResident *resident = NULL;
    uint64_t counts[2][4] = {{0}};
    uint64_t index;
    uint64_t win_three_index = UINT64_MAX;
    uint64_t lost_two_index = UINT64_MAX;
    int descriptor = mkstemp(path);
    bool ok = false;
    if (descriptor < 0)
        return false;
    close(descriptor);
    unlink(path);
    if (!eg_indexer_init(&indexer, 0, 0, 1, 1))
        goto done;
    if (!egtb_create(&database, path, eg_max_index(&indexer), 1024, &options) ||
        !egtb_generate(database, &indexer, NULL, NULL, NULL, NULL,
                       &statistics))
        goto destroy_indexer;
    for (index = 0; index < eg_position_count(&indexer); ++index) {
        unsigned side;
        for (side = 0; side < 2; ++side) {
            int16_t value;
            unsigned bucket;
            if (!egtb_get(database, index, (EgtbSide)side, &value))
                goto destroy_indexer;
            if (value == 1)
                bucket = 0;
            else if (value == -2) {
                bucket = 1;
                if (side == EGTB_WHITE_TO_MOVE)
                    lost_two_index = index;
            } else if (value == 3) {
                bucket = 2;
                if (side == EGTB_WHITE_TO_MOVE)
                    win_three_index = index;
            } else if (value == EGTB_DRAW)
                bucket = 3;
            else
                goto destroy_indexer;
            ++counts[side][bucket];
        }
    }
    if (statistics.retrograde_passes != 3 || statistics.maximum_dtm != 3 ||
        statistics.consistency_passes != 1 ||
        statistics.consistency_updates[0] != 0 ||
        statistics.consistency_updates[1] != 0 ||
        statistics.new_losses[0] != 2 || statistics.new_losses[1] != 2 ||
        statistics.new_wins[0] != 16 || statistics.new_wins[1] != 16 ||
        statistics.shortened_losses[0] != 0 ||
        statistics.shortened_losses[1] != 0 ||
        statistics.shortened_wins[0] != 0 ||
        statistics.shortened_wins[1] != 0)
        goto destroy_indexer;
    for (unsigned side = 0; side < 2; ++side) {
        if (counts[side][0] != 408 || counts[side][1] != 2 ||
            counts[side][2] != 16 || counts[side][3] != 2024)
            goto destroy_indexer;
    }
    {
        EgtbVerificationOptions repair_options = {4, 16, NULL, NULL};
        if (win_three_index == UINT64_MAX || lost_two_index == UINT64_MAX ||
            !bitmap_create(&verified_positions,
                           eg_position_count(&indexer)) ||
            !egtb_set(database, win_three_index, EGTB_WHITE_TO_MOVE, 5) ||
            !egtb_set(database, lost_two_index, EGTB_WHITE_TO_MOVE, 0) ||
            !egtb_make_consistent_threaded(
                database, &indexer, NULL, NULL, check_consistency_report,
                &report, &repair_options, &verified_positions,
                &consistency))
            goto destroy_indexer;
    }
    {
        int16_t win_value, loss_value;
        if (!egtb_get(database, win_three_index, EGTB_WHITE_TO_MOVE,
                      &win_value) ||
            !egtb_get(database, lost_two_index, EGTB_WHITE_TO_MOVE,
                      &loss_value) ||
            win_value != 3 || loss_value != -2 || report.reports < 2 ||
            !report.saw_shorter_win || !report.saw_longer_loss ||
            consistency.passes < 2 ||
            consistency.shorter_wins[EGTB_WHITE_TO_MOVE] == 0 ||
            consistency.longer_losses[EGTB_WHITE_TO_MOVE] == 0)
            goto destroy_indexer;
        if (consistency.positions_checked >=
            consistency.passes * eg_position_count(&indexer) * 2)
            goto destroy_indexer;
    }
    {
        EgtbVerificationOptions verify_options = {4, 16, NULL, NULL};
        EgtbConsistencyStatistics verification;
        if (!egtb_flush(database) || !egtb_close(database)) {
            database = NULL;
            goto destroy_indexer;
        }
        database = NULL;
        if (!egtb_compact(path, 3, 4) ||
            !egtb_open_readonly(&database, path, 4) ||
            !egtb_resident_load(&resident, database, 4))
            goto destroy_indexer;
        verify_options.resident = resident;
        if (
            !egtb_verify_consistent_threaded(
                database, &indexer, NULL, NULL, &verify_options,
                &verified_positions, &verification) ||
            verification.passes != 1 ||
            verification.positions_checked +
                    verification.positions_skipped !=
                eg_position_count(&indexer) * 2 ||
            verification.positions_checked == 0 ||
            verification.positions_skipped == 0)
            goto destroy_indexer;
        egtb_resident_destroy(resident);
        resident = NULL;
        if (!egtb_close(database)) {
            database = NULL;
            goto destroy_indexer;
        }
        database = NULL;
        if (!egtb_open_readwrite(&database, path, 4) ||
            !egtb_set(database, win_three_index, EGTB_WHITE_TO_MOVE, 5) ||
            !egtb_close(database)) {
            database = NULL;
            goto destroy_indexer;
        }
        database = NULL;
        verify_options.resident = NULL;
        if (!egtb_open_readonly(&database, path, 4) ||
            egtb_verify_consistent_threaded(
                database, &indexer, NULL, NULL, &verify_options,
                NULL, &verification))
            goto destroy_indexer;
    }
    ok = true;
destroy_indexer:
    bitmap_destroy(&verified_positions);
    eg_indexer_destroy(&indexer);
done:
    egtb_resident_destroy(resident);
    if (database != NULL && !egtb_close(database))
        ok = false;
    unlink(path);
    return ok;
}

static bool probe_long_loss(const DraughtsPosition *position, EgtbSide side,
                            void *context, int16_t *value)
{
    (void)position;
    (void)side;
    (void)context;
    *value = -300;
    return true;
}

static bool test_threaded_wk_bk_generation(unsigned threads, size_t buffer_bytes,
                                          bool long_dtm)
{
    char serial_path[] = "/tmp/ipd-generator-serial-XXXXXX";
    char threaded_path[] = "/tmp/ipd-generator-threaded-XXXXXX";
    EgtbCreateOptions create_options = {16, 20, 3};
    EgtbThreadOptions thread_options = {threads, 16, NULL, NULL, buffer_bytes};
    EgtbGenerationStatistics serial_statistics, threaded_statistics;
    EgIndexer indexer;
    Egtb *serial = NULL, *threaded = NULL;
    EgtbExternalProbe probe = long_dtm ? probe_long_loss : NULL;
    int serial_descriptor = mkstemp(serial_path);
    int threaded_descriptor = mkstemp(threaded_path);
    bool ok = false;
    if (serial_descriptor < 0 || threaded_descriptor < 0)
        goto done;
    close(serial_descriptor);
    close(threaded_descriptor);
    serial_descriptor = threaded_descriptor = -1;
    unlink(serial_path);
    unlink(threaded_path);
    if (!eg_indexer_init(&indexer, long_dtm ? 1 : 0, 0, long_dtm ? 0 : 1, 1))
        goto done;
    if (!egtb_create(&serial, serial_path, eg_max_index(&indexer), 1024,
                     &create_options) ||
        !egtb_create(&threaded, threaded_path, eg_max_index(&indexer), 1024,
                     &create_options) ||
        !egtb_generate(serial, &indexer, probe, NULL, NULL, NULL,
                       &serial_statistics) ||
        !egtb_generate_threaded(threaded, &indexer, probe, NULL, NULL, NULL,
                                &thread_options, &threaded_statistics))
        goto destroy_indexer;
    if (long_dtm && threaded_statistics.maximum_dtm <= 254)
        goto destroy_indexer;
    serial_statistics.initialization_seconds = 0.0;
    serial_statistics.backpropagation_seconds = 0.0;
    serial_statistics.compilation_seconds = 0.0;
    serial_statistics.consistency_seconds = 0.0;
    serial_statistics.final_scan_seconds = 0.0;
    serial_statistics.total_seconds = 0.0;
    memset(&serial_statistics.consistency_cache, 0,
           sizeof(serial_statistics.consistency_cache));
    threaded_statistics.initialization_seconds = 0.0;
    threaded_statistics.backpropagation_seconds = 0.0;
    threaded_statistics.compilation_seconds = 0.0;
    threaded_statistics.consistency_seconds = 0.0;
    threaded_statistics.final_scan_seconds = 0.0;
    threaded_statistics.total_seconds = 0.0;
    memset(&threaded_statistics.consistency_cache, 0,
           sizeof(threaded_statistics.consistency_cache));
    if (!long_dtm && memcmp(&serial_statistics, &threaded_statistics,
               sizeof(serial_statistics)) != 0)
        goto destroy_indexer;
    for (uint64_t index = 0; index < eg_position_count(&indexer); ++index) {
        for (unsigned side = 0; side < 2; ++side) {
            int16_t serial_value, threaded_value;
            if (!egtb_get(serial, index, (EgtbSide)side, &serial_value) ||
                !egtb_get(threaded, index, (EgtbSide)side,
                          &threaded_value) ||
                serial_value != threaded_value)
                goto destroy_indexer;
        }
    }
    if (long_dtm) {
        EgtbVerificationOptions verification = {threads, 16, NULL, NULL};
        if (!egtb_close(threaded)) {
            threaded = NULL;
            goto destroy_indexer;
        }
        threaded = NULL;
        if (!egtb_compact(threaded_path, 3, 2) ||
            !egtb_open_readonly(&threaded, threaded_path, 2) ||
            !egtb_verify_consistent_threaded(threaded, &indexer, probe, NULL,
                                             &verification, NULL, NULL))
            goto destroy_indexer;
        printf("wide frontier generation: maximum DTM=%u, serial/threaded values match\n",
               threaded_statistics.maximum_dtm);
    }
    ok = true;
destroy_indexer:
    eg_indexer_destroy(&indexer);
done:
    if (serial != NULL && !egtb_close(serial))
        ok = false;
    if (threaded != NULL && !egtb_close(threaded))
        ok = false;
    if (serial_descriptor >= 0)
        close(serial_descriptor);
    if (threaded_descriptor >= 0)
        close(threaded_descriptor);
    unlink(serial_path);
    unlink(threaded_path);
    return ok;
}

static bool run_case(unsigned white_men, unsigned black_men,
                     unsigned white_kings, unsigned black_kings,
                     uint64_t expected_positions,
                     const uint64_t expected_lost[2],
                     const uint64_t expected_won[2],
                     const uint64_t expected_unknown[2],
                     EgtbBacktrackStatistics *backtrack,
                     EgtbWinBacktrackStatistics *win_backtrack,
                     EgtbLossBacktrackStatistics *general_loss_backtrack,
                     bool test_shortening)
{
    char path[] = "/tmp/ipd-generator-test-XXXXXX";
    EgtbCreateOptions options = {16, 20, 3};
    EgtbInitializationStatistics statistics;
    EgIndexer indexer;
    Egtb *database = NULL;
    uint64_t index;
    int descriptor = mkstemp(path);
    bool ok = false;
    if (descriptor < 0)
        return false;
    close(descriptor);
    unlink(path);
    if (!eg_indexer_init(&indexer, white_men, black_men,
                         white_kings, black_kings))
        goto done;
    if (eg_position_count(&indexer) != expected_positions ||
        !egtb_create(&database, path, expected_positions - 1, 1024, &options) ||
        !egtb_initialize_terminal_positions(database, &indexer, &statistics))
        goto destroy_indexer;
    if (statistics.positions != expected_positions)
        goto destroy_indexer;
    for (unsigned side = 0; side < 2; ++side) {
        if (statistics.lost_in_zero[side] != expected_lost[side] ||
            statistics.won_in_one[side] != expected_won[side] ||
            statistics.unknown[side] != expected_unknown[side] ||
            statistics.lost_in_zero[side] + statistics.won_in_one[side] +
                    statistics.unknown[side] != expected_positions)
            goto destroy_indexer;
    }
    if (!egtb_backtrack_won_in_one(database, &indexer, backtrack))
        goto destroy_indexer;
    if (!egtb_backtrack_lost_in_two(database, &indexer, win_backtrack))
        goto destroy_indexer;
    if (test_shortening) {
        EgtbWinBacktrackStatistics shortened;
        bool changed = false;
        for (index = 0; index < expected_positions && !changed; ++index) {
            unsigned side;
            for (side = 0; side < 2; ++side) {
                int16_t value;
                if (!egtb_get(database, index, (EgtbSide)side, &value))
                    goto destroy_indexer;
                if (value == 3) {
                    if (!egtb_set(database, index, (EgtbSide)side, 5))
                        goto destroy_indexer;
                    changed = true;
                    break;
                }
            }
        }
        if (!changed ||
            !egtb_backtrack_lost_in_two(database, &indexer, &shortened) ||
            shortened.won_in_three[0] + shortened.won_in_three[1] != 1 ||
            shortened.shortened_wins[0] + shortened.shortened_wins[1] != 1)
            goto destroy_indexer;
    }
    if (!egtb_backtrack_wins_to_losses(database, &indexer, 3,
                                        general_loss_backtrack))
        goto destroy_indexer;
    for (index = 0; index < expected_positions; ++index) {
        unsigned side;
        for (side = 0; side < 2; ++side) {
            int16_t value;
            if (!egtb_get(database, index, (EgtbSide)side, &value) ||
                (value != EGTB_DRAW && value != 0 && value != 1 &&
                 value != -2 && value != 3 && value != -4))
                goto destroy_indexer;
        }
    }
    ok = true;
destroy_indexer:
    eg_indexer_destroy(&indexer);
done:
    if (database != NULL && !egtb_close(database))
        ok = false;
    unlink(path);
    return ok;
}

int main(void)
{
    static const uint64_t wk_bk_lost[2] = {0, 0};
    static const uint64_t wk_bk_won[2] = {408, 408};
    static const uint64_t wk_bk_unknown[2] = {2042, 2042};
    static const uint64_t wk_lost[2] = {0, 50};
    static const uint64_t wk_won[2] = {50, 0};
    static const uint64_t wk_unknown[2] = {0, 0};
    EgtbBacktrackStatistics wk_bk_backtrack, wk_backtrack;
    EgtbWinBacktrackStatistics wk_bk_win_backtrack, wk_win_backtrack;
    EgtbLossBacktrackStatistics wk_bk_loss_backtrack, wk_loss_backtrack;
    if (!run_case(0, 0, 1, 1, 2450, wk_bk_lost, wk_bk_won,
                  wk_bk_unknown, &wk_bk_backtrack,
                  &wk_bk_win_backtrack, &wk_bk_loss_backtrack, true)) {
        fprintf(stderr, "WK-BK initialization test failed: %s / %s\n",
                egtb_generator_last_error(), egtb_last_error());
        return EXIT_FAILURE;
    }
    if (!run_case(0, 0, 1, 0, 50, wk_lost, wk_won, wk_unknown,
                  &wk_backtrack, &wk_win_backtrack, &wk_loss_backtrack,
                  false)) {
        fprintf(stderr, "zero-piece terminal test failed: %s / %s\n",
                egtb_generator_last_error(), egtb_last_error());
        return EXIT_FAILURE;
    }
    if (!test_parameterized_win_backtrack()) {
        fprintf(stderr, "parameterized win-to-loss backtrack test failed\n");
        return EXIT_FAILURE;
    }
    if (!test_external_promotion_initialization()) {
        fprintf(stderr, "external promotion initialization test failed\n");
        return EXIT_FAILURE;
    }
    if (!test_external_promotion_backtrack()) {
        fprintf(stderr, "external promotion backtrack test failed: %s\n",
                egtb_generator_last_error());
        return EXIT_FAILURE;
    }
    if (!test_complete_wk_bk_generation()) {
        fprintf(stderr, "complete WK-BK generation test failed: %s\n",
                egtb_generator_last_error());
        return EXIT_FAILURE;
    }
    /* One-page buffers force replay across batches, including the partial
     * last page. Also exercise the default budget and excess worker count. */
    if (!test_threaded_wk_bk_generation(1, 2048, false) ||
        !test_threaded_wk_bk_generation(2, 4096, false) ||
        !test_threaded_wk_bk_generation(4, 0, false) ||
        !test_threaded_wk_bk_generation(2, 4096, true)) {
        fprintf(stderr, "threaded WK-BK equivalence test failed: %s / %s\n",
                egtb_generator_last_error(), egtb_last_error());
        return EXIT_FAILURE;
    }
    if (wk_bk_backtrack.won_in_one_sources[0] != 408 ||
        wk_bk_backtrack.won_in_one_sources[1] != 408 ||
        wk_bk_backtrack.predecessor_candidates[0] != 3168 ||
        wk_bk_backtrack.predecessor_candidates[1] != 3168 ||
        wk_bk_backtrack.lost_in_two[0] != 2 ||
        wk_bk_backtrack.lost_in_two[1] != 2 ||
        wk_backtrack.won_in_one_sources[0] != 0 ||
        wk_backtrack.won_in_one_sources[1] != 50 ||
        wk_backtrack.predecessor_candidates[0] != 0 ||
        wk_backtrack.predecessor_candidates[1] != 0 ||
        wk_backtrack.lost_in_two[0] != 0 ||
        wk_backtrack.lost_in_two[1] != 0 ||
        wk_bk_win_backtrack.lost_in_two_sources[0] != 2 ||
        wk_bk_win_backtrack.lost_in_two_sources[1] != 2 ||
        wk_bk_win_backtrack.predecessor_candidates[0] != 16 ||
        wk_bk_win_backtrack.predecessor_candidates[1] != 16 ||
        wk_bk_win_backtrack.won_in_three[0] != 16 ||
        wk_bk_win_backtrack.won_in_three[1] != 16 ||
        wk_bk_win_backtrack.shortened_wins[0] != 0 ||
        wk_bk_win_backtrack.shortened_wins[1] != 0 ||
        wk_win_backtrack.lost_in_two_sources[0] != 0 ||
        wk_win_backtrack.lost_in_two_sources[1] != 0 ||
        wk_win_backtrack.predecessor_candidates[0] != 0 ||
        wk_win_backtrack.predecessor_candidates[1] != 0 ||
        wk_win_backtrack.won_in_three[0] != 0 ||
        wk_win_backtrack.won_in_three[1] != 0 ||
        wk_bk_loss_backtrack.won_sources[0] != 16 ||
        wk_bk_loss_backtrack.won_sources[1] != 16 ||
        wk_bk_loss_backtrack.predecessor_candidates[0] != 0 ||
        wk_bk_loss_backtrack.predecessor_candidates[1] != 0 ||
        wk_bk_loss_backtrack.losses[0] != 0 ||
        wk_bk_loss_backtrack.losses[1] != 0 ||
        wk_loss_backtrack.won_sources[0] != 0 ||
        wk_loss_backtrack.won_sources[1] != 0 ||
        wk_loss_backtrack.losses[0] != 0 ||
        wk_loss_backtrack.losses[1] != 0) {
        fprintf(stderr, "won-in-one backtrack statistics mismatch\n");
        return EXIT_FAILURE;
    }
    printf("EGTB initialization/backtrack tests: PASS\n");
    printf("WK-BK: positions=2450, WTM/BTM won-1=408, unknown=2042\n");
    printf("WK-none: WTM won-1=50, BTM lost-0=50\n");
    printf("WK-BK lost-2: WTM=%" PRIu64 " BTM=%" PRIu64
           " candidates=%" PRIu64 "/%" PRIu64 "\n",
           wk_bk_backtrack.lost_in_two[0], wk_bk_backtrack.lost_in_two[1],
           wk_bk_backtrack.predecessor_candidates[0],
           wk_bk_backtrack.predecessor_candidates[1]);
    printf("WK-none lost-2: WTM=%" PRIu64 " BTM=%" PRIu64 "\n",
           wk_backtrack.lost_in_two[0], wk_backtrack.lost_in_two[1]);
    printf("WK-BK won-3: WTM=%" PRIu64 " BTM=%" PRIu64
           " candidates=%" PRIu64 "/%" PRIu64 "\n",
           wk_bk_win_backtrack.won_in_three[0],
           wk_bk_win_backtrack.won_in_three[1],
           wk_bk_win_backtrack.predecessor_candidates[0],
           wk_bk_win_backtrack.predecessor_candidates[1]);
    printf("WK-BK lost-4: WTM=%" PRIu64 " BTM=%" PRIu64
           " candidates=%" PRIu64 "/%" PRIu64 "\n",
           wk_bk_loss_backtrack.losses[0],
           wk_bk_loss_backtrack.losses[1],
           wk_bk_loss_backtrack.predecessor_candidates[0],
           wk_bk_loss_backtrack.predecessor_candidates[1]);
    return EXIT_SUCCESS;
}
