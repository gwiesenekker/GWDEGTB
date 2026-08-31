#define _POSIX_C_SOURCE 200809L

#include "sliced.h"

#include "revision.h"
#include "progress.h"

#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { SLICE_ROWS = 10, SLICE_MANIFEST_VERSION = 1 };

typedef struct {
    Egtb *database;
    EgtbView *view;
    EgIndexer indexer;
    bool indexer_initialized;
} SliceProbeEntry;

typedef struct {
    EgtbMaterial material;
    const char *work_directory;
    size_t cache_pages;
    EgtbExternalProbe fallback;
    void *fallback_context;
    SliceProbeEntry entries[SLICE_ROWS][SLICE_ROWS];
    char error[256];
} SliceProbeContext;

typedef struct {
    int white_row;
    int black_row;
    EgIndexer indexer;
    Egtb *database;
    EgtbView *view;
    EgtbSequentialReader reader;
    uint64_t next_local;
    uint64_t count;
    uint64_t full_index;
    uint64_t previous_full_index;
    int16_t white_to_move;
    int16_t black_to_move;
    bool has_previous;
} MergeSlice;

static _Thread_local char sliced_error[256];

static bool sliced_fail(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(sliced_error, sizeof(sliced_error), format, arguments);
    va_end(arguments);
    return false;
}

const char *egtb_sliced_last_error(void)
{
    return sliced_error;
}

static bool same_material(const EgtbMaterial *a, const EgtbMaterial *b)
{
    return a->white_kings == b->white_kings &&
           a->white_men == b->white_men &&
           a->black_kings == b->black_kings &&
           a->black_men == b->black_men;
}

static bool make_work_directory(char *buffer, size_t size, const char *path)
{
    int count = snprintf(buffer, size, "%s.work", path);
    return count > 0 && (size_t)count < size;
}

static bool make_slice_path(char *buffer, size_t size, const char *directory,
                            int white_row, int black_row,
                            const char *suffix)
{
    int count = snprintf(buffer, size, "%s/slice-w%02d-b%02d.dtm%s",
                         directory, white_row < 0 ? 0 : white_row + 1,
                         black_row < 0 ? 0 : black_row + 1, suffix);
    return count > 0 && (size_t)count < size;
}

static bool initialize_slice_indexer(EgIndexer *indexer,
                                     const EgtbMaterial *material,
                                     int white_row, int black_row)
{
    return eg_slice_indexer_init(indexer, material->white_men,
                                 material->black_men,
                                 material->white_kings,
                                 material->black_kings,
                                 white_row, black_row);
}

static void close_probe_context(SliceProbeContext *context)
{
    for (int white = 0; white < SLICE_ROWS; ++white)
        for (int black = 0; black < SLICE_ROWS; ++black) {
            SliceProbeEntry *entry = &context->entries[white][black];
            if (entry->view != NULL)
                egtb_view_close(entry->view);
            if (entry->database != NULL)
                egtb_close(entry->database);
            if (entry->indexer_initialized)
                eg_indexer_destroy(&entry->indexer);
            memset(entry, 0, sizeof(*entry));
        }
}

static bool open_probe_slice(SliceProbeContext *context, int white_row,
                             int black_row, SliceProbeEntry **result)
{
    int white_slot = white_row < 0 ? 0 : white_row;
    int black_slot = black_row < 0 ? 0 : black_row;
    SliceProbeEntry *entry = &context->entries[white_slot][black_slot];
    char path[512];
    if (entry->database != NULL) {
        *result = entry;
        return true;
    }
    if (!make_slice_path(path, sizeof(path), context->work_directory,
                         white_row, black_row, "") ||
        !initialize_slice_indexer(&entry->indexer, &context->material,
                                  white_row, black_row)) {
        snprintf(context->error, sizeof(context->error),
                 "cannot initialize slice dependency");
        return false;
    }
    entry->indexer_initialized = true;
    if (!egtb_open_readonly(&entry->database, path, 1) ||
        egtb_maximum_index(entry->database) !=
            eg_max_index(&entry->indexer) ||
        !egtb_view_create(&entry->view, entry->database,
                          context->cache_pages, false)) {
        snprintf(context->error, sizeof(context->error),
                 "cannot open completed slice %.160s: %.70s", path,
                 egtb_last_error());
        return false;
    }
    *result = entry;
    return true;
}

static bool slice_probe(const DraughtsPosition *position, EgtbSide side,
                        void *opaque, int16_t *value)
{
    SliceProbeContext *context = opaque;
    EgtbMaterial requested = egtb_position_material(position);
    if (same_material(&requested, &context->material)) {
        EgPosition indexed;
        SliceProbeEntry *entry;
        uint64_t index;
        int white_row, black_row;
        indexed.white_men = position->white_men;
        indexed.black_men = position->black_men;
        indexed.white_kings = position->white_kings;
        indexed.black_kings = position->black_kings;
        eg_position_slice(&indexed, &white_row, &black_row);
        if (!open_probe_slice(context, white_row, black_row, &entry) ||
            !eg_position_to_index(&entry->indexer, &indexed, &index) ||
            !egtb_view_get(entry->view, index, side, value)) {
            if (context->error[0] == '\0')
                snprintf(context->error, sizeof(context->error),
                         "cannot query completed slice: %.180s",
                         egtb_last_error());
            return false;
        }
        return true;
    }
    if (context->fallback == NULL ||
        !context->fallback(position, side, context->fallback_context, value)) {
        snprintf(context->error, sizeof(context->error),
                 "cannot query external material dependency");
        return false;
    }
    return true;
}

static void add_generation_statistics(EgtbGenerationStatistics *total,
                                      const EgtbGenerationStatistics *part)
{
    unsigned side;
    total->initialization.positions += part->initialization.positions;
    for (side = 0; side < 2; ++side) {
        total->initialization.legal_moves[side] +=
            part->initialization.legal_moves[side];
        total->initialization.lost_in_zero[side] +=
            part->initialization.lost_in_zero[side];
        total->initialization.won_in_one[side] +=
            part->initialization.won_in_one[side];
        total->initialization.external_wins[side] +=
            part->initialization.external_wins[side];
        total->initialization.external_losses[side] +=
            part->initialization.external_losses[side];
        total->initialization.unknown[side] +=
            part->initialization.unknown[side];
        total->new_losses[side] += part->new_losses[side];
        total->new_wins[side] += part->new_wins[side];
        total->shortened_losses[side] += part->shortened_losses[side];
        total->shortened_wins[side] += part->shortened_wins[side];
        total->consistency_updates[side] += part->consistency_updates[side];
    }
    if (part->retrograde_passes > total->retrograde_passes)
        total->retrograde_passes = part->retrograde_passes;
    if (part->consistency_passes > total->consistency_passes)
        total->consistency_passes = part->consistency_passes;
    if (part->maximum_dtm > total->maximum_dtm)
        total->maximum_dtm = part->maximum_dtm;
    total->initialization_seconds += part->initialization_seconds;
    total->backpropagation_seconds += part->backpropagation_seconds;
    total->compilation_seconds += part->compilation_seconds;
    total->consistency_seconds += part->consistency_seconds;
    total->final_scan_seconds += part->final_scan_seconds;
    total->total_seconds += part->total_seconds;
    total->consistency_cache.lookups += part->consistency_cache.lookups;
    total->consistency_cache.hits += part->consistency_cache.hits;
    total->consistency_cache.misses += part->consistency_cache.misses;
    total->consistency_cache.decompressions +=
        part->consistency_cache.decompressions;
    total->consistency_cache.dirty_evictions +=
        part->consistency_cache.dirty_evictions;
    total->consistency_cache.compressed_writes +=
        part->consistency_cache.compressed_writes;
}

static bool validate_manifest(const char *directory,
                              const EgtbMaterial *material,
                              uint64_t full_positions, uint32_t page_size,
                              EgtbGenerationStatistics
                                  slice_statistics[SLICE_ROWS][SLICE_ROWS])
{
    char path[512], magic[64];
    FILE *file;
    unsigned version, wk, wm, bk, bm, stored_page;
    uint64_t stored_positions;
    snprintf(path, sizeof(path), "%s/manifest", directory);
    file = fopen(path, "r");
    if (file == NULL)
        return errno == ENOENT;
    if (fscanf(file, "%63s %u\n", magic, &version) != 2 ||
        strcmp(magic, "GWDEGTB-SLICES") != 0 ||
        version != SLICE_MANIFEST_VERSION ||
        fscanf(file, "material %u %u %u %u\n", &wk, &wm, &bk, &bm) != 4 ||
        fscanf(file, "positions %" SCNu64 "\n", &stored_positions) != 1 ||
        fscanf(file, "page-size %u\n", &stored_page) != 1) {
        fclose(file);
        return sliced_fail("invalid slice manifest %s", path);
    }
    if (wk != material->white_kings || wm != material->white_men ||
        bk != material->black_kings || bm != material->black_men ||
        stored_positions != full_positions || stored_page != page_size) {
        fclose(file);
        return sliced_fail("slice manifest does not match this generation");
    }
    {
        char line[512];
        while (fgets(line, sizeof(line), file) != NULL) {
            int white, black;
            EgtbGenerationStatistics part = {0};
            if (sscanf(line,
                       "complete %d %d %" SCNu64 " %hu %" SCNu64
                       " %" SCNu64 " %" SCNu64
                       " %lf %lf %lf %lf %lf %lf",
                       &white, &black, &part.retrograde_passes,
                       &part.maximum_dtm, &part.consistency_passes,
                       &part.consistency_updates[0],
                       &part.consistency_updates[1],
                       &part.initialization_seconds,
                       &part.backpropagation_seconds,
                       &part.compilation_seconds,
                       &part.consistency_seconds,
                       &part.final_scan_seconds,
                       &part.total_seconds) == 13 &&
                white >= 0 && white < SLICE_ROWS &&
                black >= 0 && black < SLICE_ROWS)
                slice_statistics[white][black] = part;
        }
    }
    fclose(file);
    return true;
}

static bool write_manifest(const char *directory,
                           const EgtbMaterial *material,
                           uint64_t full_positions, uint32_t page_size,
                           const bool completed[SLICE_ROWS][SLICE_ROWS],
                           const EgtbGenerationStatistics
                               slice_statistics[SLICE_ROWS][SLICE_ROWS])
{
    char path[512], temporary[512];
    FILE *file;
    snprintf(path, sizeof(path), "%s/manifest", directory);
    snprintf(temporary, sizeof(temporary), "%s/manifest.incomplete", directory);
    file = fopen(temporary, "w");
    if (file == NULL)
        return sliced_fail("cannot write slice manifest: %s", strerror(errno));
    fprintf(file, "GWDEGTB-SLICES %u\n", SLICE_MANIFEST_VERSION);
    fprintf(file, "material %u %u %u %u\n", material->white_kings,
            material->white_men, material->black_kings, material->black_men);
    fprintf(file, "positions %" PRIu64 "\n", full_positions);
    fprintf(file, "page-size %u\n", page_size);
    fprintf(file, "revision %s\n", gwdegtb_revision);
    for (int white = 0; white < SLICE_ROWS; ++white)
        for (int black = 0; black < SLICE_ROWS; ++black)
            if (completed[white][black])
                fprintf(file,
                        "complete %d %d %" PRIu64 " %u %" PRIu64
                        " %" PRIu64 " %" PRIu64
                        " %.17g %.17g %.17g %.17g %.17g %.17g\n",
                        white, black,
                        slice_statistics[white][black].retrograde_passes,
                        slice_statistics[white][black].maximum_dtm,
                        slice_statistics[white][black].consistency_passes,
                        slice_statistics[white][black].consistency_updates[0],
                        slice_statistics[white][black].consistency_updates[1],
                        slice_statistics[white][black].initialization_seconds,
                        slice_statistics[white][black].backpropagation_seconds,
                        slice_statistics[white][black].compilation_seconds,
                        slice_statistics[white][black].consistency_seconds,
                        slice_statistics[white][black].final_scan_seconds,
                        slice_statistics[white][black].total_seconds);
    if (fflush(file) != 0 || fsync(fileno(file)) != 0 || fclose(file) != 0 ||
        rename(temporary, path) != 0)
        return sliced_fail("cannot commit slice manifest: %s", strerror(errno));
    return true;
}

static bool completed_slice_valid(const char *directory,
                                  const EgtbMaterial *material,
                                  int white_row, int black_row)
{
    char path[512];
    EgIndexer indexer = {0};
    Egtb *database = NULL;
    EgtbView *view = NULL;
    EgtbSequentialReader reader;
    bool valid = false;
    if (!make_slice_path(path, sizeof(path), directory,
                         white_row, black_row, "") ||
        access(path, F_OK) != 0)
        return false;
    if (initialize_slice_indexer(&indexer, material, white_row, black_row) &&
        egtb_open_readonly(&database, path, 1) &&
        egtb_maximum_index(database) == eg_max_index(&indexer) &&
        egtb_view_create(&view, database, 1, false) &&
        egtb_sequential_reader_init(&reader, view, 0,
                                    eg_position_count(&indexer))) {
        valid = true;
        for (uint64_t index = 0; index < eg_position_count(&indexer);
             ++index) {
            int16_t white, black;
            if (!egtb_sequential_reader_next(&reader, &white, &black)) {
                valid = false;
                break;
            }
        }
    }
    if (view != NULL)
        egtb_view_close(view);
    if (database != NULL)
        egtb_close(database);
    eg_indexer_destroy(&indexer);
    return valid;
}

/* Frontier rows must leave room for all men behind them. With six or
 * more men the last one-row slice is empty; never create, probe or merge
 * a database for such a slice. The two man regions together have at least
 * ten squares, so these per-colour bounds suffice through eight pieces. */
static int last_white_slice_row(const EgtbMaterial *material)
{
    return material->white_men == 0 ? -1
        : 10 - (int)((material->white_men + 4) / 5);
}

static int last_black_slice_row(const EgtbMaterial *material)
{
    return material->black_men == 0 ? -1
        : (int)((material->black_men + 4) / 5) - 1;
}

static bool prepare_workspace(char *directory, size_t directory_size,
                              const char *path, const EgtbMaterial *material,
                              uint64_t full_positions, uint32_t page_size,
                              bool completed[SLICE_ROWS][SLICE_ROWS],
                              EgtbGenerationStatistics
                                  slice_statistics[SLICE_ROWS][SLICE_ROWS])
{
    int white_first = material->white_men == 0 ? -1 : 1;
    int white_last = last_white_slice_row(material);
    int black_first = material->black_men == 0 ? -1 : 8;
    int black_last = last_black_slice_row(material);
    struct stat status;
    memset(completed, 0, sizeof(bool) * SLICE_ROWS * SLICE_ROWS);
    memset(slice_statistics, 0,
           sizeof(EgtbGenerationStatistics) * SLICE_ROWS * SLICE_ROWS);
    if (!make_work_directory(directory, directory_size, path))
        return sliced_fail("slice workspace path is too long");
    if (stat(directory, &status) != 0) {
        if (errno != ENOENT || mkdir(directory, 0777) != 0)
            return sliced_fail("cannot create slice workspace %s: %s",
                               directory, strerror(errno));
    } else if (!S_ISDIR(status.st_mode)) {
        return sliced_fail("slice workspace path is not a directory");
    }
    if (!validate_manifest(directory, material, full_positions, page_size,
                           slice_statistics))
        return false;
    for (int white = white_first;; ++white) {
        for (int black = black_first;; --black) {
            int ws = white < 0 ? 0 : white;
            int bs = black < 0 ? 0 : black;
            completed[ws][bs] = completed_slice_valid(
                directory, material, white, black);
            if (black == black_last)
                break;
        }
        if (white == white_last)
            break;
    }
    return write_manifest(directory, material, full_positions, page_size,
                          completed, slice_statistics);
}

static bool generate_one_slice(const char *directory,
                               const EgtbMaterial *material,
                               int white_row, int black_row,
                               const EgtbSlicedOptions *options,
                               SliceProbeContext *contexts,
                               void **probe_contexts,
                               EgtbGenerationStatistics *statistics)
{
    char final_path[512], incomplete_path[512];
    EgIndexer indexer = {0};
    Egtb *database = NULL;
    Bitmap verified = {0};
    EgtbGenerationStatistics generated = {0};
    EgtbConsistencyStatistics verification = {0};
    EgtbCreateOptions create_options = {
        options->writable_cache_pages, options->reserve_percent,
        options->compression_level
    };
    bool ok = false;
    if (!make_slice_path(final_path, sizeof(final_path), directory,
                         white_row, black_row, "") ||
        !make_slice_path(incomplete_path, sizeof(incomplete_path), directory,
                         white_row, black_row, ".incomplete") ||
        !initialize_slice_indexer(&indexer, material,
                                  white_row, black_row))
        return sliced_fail("cannot initialize generation slice");
    if (!options->quiet)
        egtb_progress_log("starting slice white-row=%d black-row=%d positions=%" PRIu64 "\n",
                          white_row < 0 ? 0 : white_row + 1,
                          black_row < 0 ? 0 : black_row + 1,
                          eg_position_count(&indexer));
    unlink(incomplete_path);
    for (unsigned i = 0; i < options->thread_count; ++i) {
        contexts[i].error[0] = '\0';
        contexts[i].material = *material;
        contexts[i].work_directory = directory;
        contexts[i].cache_pages = options->slice_read_cache_pages;
        contexts[i].fallback = options->external_probe;
        contexts[i].fallback_context = options->external_contexts != NULL
                                           ? options->external_contexts[i]
                                           : options->external_context;
        probe_contexts[i] = &contexts[i];
    }
    if (!egtb_create(&database, incomplete_path,
                     eg_position_count(&indexer) - 1,
                     options->page_size, &create_options)) {
        sliced_fail("cannot create slice: %s", egtb_last_error());
        goto done;
    }
    {
        EgtbThreadOptions thread_options = {
            options->thread_count, options->writable_cache_pages,
            probe_contexts, &verified, options->compilation_buffer_bytes
        };
        if (!egtb_generate_threaded(
                database, &indexer, slice_probe, &contexts[0],
                options->reporter, options->reporter_context,
                &thread_options, &generated)) {
            const char *detail = "";
            for (unsigned i = 0; i < options->thread_count; ++i)
                if (contexts[i].error[0] != '\0') {
                    detail = contexts[i].error;
                    break;
                }
            sliced_fail("slice generation failed: %s%s%s",
                        egtb_generator_last_error(), *detail ? ": " : "",
                        detail);
            goto done;
        }
    }
    if (!egtb_close(database)) {
        database = NULL;
        sliced_fail("cannot close generated slice: %s", egtb_last_error());
        goto done;
    }
    database = NULL;
    if (!egtb_compact(incomplete_path, options->compression_level,
                      options->slice_read_cache_pages) ||
        !egtb_open_readonly(&database, incomplete_path, 1)) {
        sliced_fail("cannot compact generated slice: %s", egtb_last_error());
        goto done;
    }
    {
        EgtbVerificationOptions verify_options = {
            options->thread_count, options->verification_cache_pages,
            probe_contexts, NULL
        };
        if (!egtb_verify_consistent_threaded(
                database, &indexer, slice_probe, &contexts[0],
                &verify_options, verified.words != NULL ? &verified : NULL,
                &verification)) {
            const char *detail = "";
            for (unsigned i = 0; i < options->thread_count; ++i)
                if (contexts[i].error[0] != '\0') {
                    detail = contexts[i].error;
                    break;
                }
            sliced_fail("generated slice failed verification: %s%s%s",
                        egtb_generator_last_error(), *detail ? ": " : "",
                        detail);
            goto done;
        }
    }
    if (!egtb_close(database)) {
        database = NULL;
        sliced_fail("cannot close verified slice: %s", egtb_last_error());
        goto done;
    }
    database = NULL;
    if (rename(incomplete_path, final_path) != 0) {
        sliced_fail("cannot commit generated slice: %s", strerror(errno));
        goto done;
    }
    *statistics = generated;
    if (!options->quiet) {
        egtb_progress_log("completed slice white-row=%d black-row=%d positions=%" PRIu64
               " passes=%" PRIu64 "\n",
               white_row < 0 ? 0 : white_row + 1,
               black_row < 0 ? 0 : black_row + 1,
               eg_position_count(&indexer), generated.retrograde_passes);
        fflush(stdout);
    }
    ok = true;
done:
    if (database != NULL)
        egtb_close(database);
    bitmap_destroy(&verified);
    for (unsigned i = 0; i < options->thread_count; ++i)
        close_probe_context(&contexts[i]);
    eg_indexer_destroy(&indexer);
    if (!ok)
        unlink(incomplete_path);
    return ok;
}

static bool load_merge_entry(MergeSlice *slice, const EgIndexer *full_indexer)
{
    EgPosition position;
    if (slice->next_local >= slice->count)
        return false;
    if (!eg_index_to_position(&slice->indexer, slice->next_local, &position) ||
        !eg_position_to_index(full_indexer, &position, &slice->full_index) ||
        !egtb_sequential_reader_next(&slice->reader,
                                     &slice->white_to_move,
                                     &slice->black_to_move))
        return sliced_fail("cannot read slice during final compilation");
    if (slice->has_previous && slice->full_index <= slice->previous_full_index)
        return sliced_fail("slice-to-full index mapping is not monotonic");
    slice->previous_full_index = slice->full_index;
    slice->has_previous = true;
    ++slice->next_local;
    return true;
}

static bool less_slice(const MergeSlice *slices, unsigned left,
                       unsigned right)
{
    return slices[left].full_index < slices[right].full_index;
}

static void heap_push(unsigned *heap, unsigned *count, unsigned value,
                      const MergeSlice *slices)
{
    unsigned position = (*count)++;
    while (position != 0) {
        unsigned parent = (position - 1) / 2;
        if (!less_slice(slices, value, heap[parent]))
            break;
        heap[position] = heap[parent];
        position = parent;
    }
    heap[position] = value;
}

static unsigned heap_pop(unsigned *heap, unsigned *count,
                         const MergeSlice *slices)
{
    unsigned result = heap[0];
    unsigned value = heap[--*count];
    unsigned position = 0;
    while (position * 2 + 1 < *count) {
        unsigned child = position * 2 + 1;
        if (child + 1 < *count &&
            less_slice(slices, heap[child + 1], heap[child]))
            ++child;
        if (!less_slice(slices, heap[child], value))
            break;
        heap[position] = heap[child];
        position = child;
    }
    if (*count != 0)
        heap[position] = value;
    return result;
}

static void close_merge_slices(MergeSlice *slices, unsigned count)
{
    for (unsigned i = 0; i < count; ++i) {
        if (slices[i].view != NULL)
            egtb_view_close(slices[i].view);
        if (slices[i].database != NULL)
            egtb_close(slices[i].database);
        eg_indexer_destroy(&slices[i].indexer);
    }
}

static bool compile_slices(Egtb **out, const char *path,
                           const char *directory,
                           const EgtbMaterial *material,
                           const EgIndexer *full_indexer,
                           const EgtbSlicedOptions *options)
{
    MergeSlice slices[81];
    unsigned heap[81], heap_count = 0, slice_count = 0;
    char temporary[512];
    Egtb *output = NULL;
    EgtbCreateOptions create_options = {64, 0, options->compression_level};
    uint64_t expected = 0, progress_pending = 0;
    int white_first = material->white_men == 0 ? -1 : 1;
    int white_last = last_white_slice_row(material);
    int black_first = material->black_men == 0 ? -1 : 8;
    int black_last = last_black_slice_row(material);
    bool ok = false;
    memset(slices, 0, sizeof(slices));
    snprintf(temporary, sizeof(temporary), "%s.incomplete", path);
    unlink(temporary);
    for (int white = white_first;; ++white) {
        for (int black = black_first;; --black) {
            unsigned slice_index = slice_count++;
            MergeSlice *slice = &slices[slice_index];
            char slice_path[512];
            slice->white_row = white;
            slice->black_row = black;
            if (!make_slice_path(slice_path, sizeof(slice_path), directory,
                                 white, black, "") ||
                !initialize_slice_indexer(&slice->indexer, material,
                                          white, black) ||
                !egtb_open_readonly(&slice->database, slice_path, 1) ||
                egtb_maximum_index(slice->database) !=
                    eg_max_index(&slice->indexer) ||
                !egtb_view_create(&slice->view, slice->database, 1, false)) {
                sliced_fail("cannot open slice for compilation: %s",
                            egtb_last_error());
                goto done;
            }
            slice->count = eg_position_count(&slice->indexer);
            if (!egtb_sequential_reader_init(&slice->reader, slice->view,
                                             0, slice->count) ||
                !load_merge_entry(slice, full_indexer))
                goto done;
            heap_push(heap, &heap_count, slice_index, slices);
            if (black == black_last)
                break;
        }
        if (white == white_last)
            break;
    }
    if (!egtb_create(&output, temporary,
                     eg_position_count(full_indexer) - 1,
                     options->page_size, &create_options)) {
        sliced_fail("cannot create compiled full database: %s",
                    egtb_last_error());
        goto done;
    }
    egtb_progress_begin("slice merge", eg_position_count(full_indexer), "positions");
    while (heap_count != 0) {
        unsigned selected = heap_pop(heap, &heap_count, slices);
        MergeSlice *slice = &slices[selected];
        if (slice->full_index != expected ||
            !egtb_set_pair(output, expected, slice->white_to_move,
                           slice->black_to_move)) {
            sliced_fail("slice merge index mismatch at %" PRIu64, expected);
            goto done;
        }
        ++expected;
        egtb_progress_tick(&progress_pending);
        if (slice->next_local < slice->count) {
            if (!load_merge_entry(slice, full_indexer))
                goto done;
            heap_push(heap, &heap_count, selected, slices);
        }
    }
    if (expected != eg_position_count(full_indexer)) {
        sliced_fail("slice merge produced %" PRIu64 " of %" PRIu64
                    " positions", expected, eg_position_count(full_indexer));
        goto done;
    }
    if (!egtb_close(output)) {
        output = NULL;
        sliced_fail("cannot close compiled full database: %s",
                    egtb_last_error());
        goto done;
    }
    output = NULL;
    if (rename(temporary, path) != 0 ||
        !egtb_open_readwrite(&output, path, 64)) {
        sliced_fail("cannot commit compiled full database: %s",
                    strerror(errno));
        goto done;
    }
    *out = output;
    output = NULL;
    ok = true;
done:
    egtb_progress_flush(&progress_pending);
    egtb_progress_end(ok);
    if (output != NULL)
        egtb_close(output);
    close_merge_slices(slices, slice_count);
    if (!ok)
        unlink(temporary);
    return ok;
}

bool egtb_generate_sliced(Egtb **out, const char *path,
                          const EgtbMaterial *material,
                          const EgIndexer *full_indexer,
                          const EgtbSlicedOptions *options,
                          EgtbGenerationStatistics *statistics)
{
    char directory[512];
    bool completed[SLICE_ROWS][SLICE_ROWS];
    EgtbGenerationStatistics
        slice_statistics[SLICE_ROWS][SLICE_ROWS];
    SliceProbeContext *contexts = NULL;
    void **probe_contexts = NULL;
    EgtbGenerationStatistics total = {0};
    int white_first, white_last, black_first, black_last;
    bool ok = false;
    if (out == NULL || path == NULL || material == NULL ||
        full_indexer == NULL || options == NULL ||
        options->thread_count == 0 ||
        (material->white_men == 0 && material->black_men == 0))
        return sliced_fail("invalid sliced generation arguments");
    *out = NULL;
    if (access(path, F_OK) == 0)
        return sliced_fail("final database already exists: %s", path);
    if (!prepare_workspace(directory, sizeof(directory), path, material,
                           eg_position_count(full_indexer), options->page_size,
                           completed, slice_statistics))
        return false;
    contexts = calloc(options->thread_count, sizeof(*contexts));
    probe_contexts = calloc(options->thread_count, sizeof(*probe_contexts));
    if (contexts == NULL || probe_contexts == NULL) {
        sliced_fail("cannot allocate sliced probe contexts");
        goto done;
    }
    white_first = material->white_men == 0 ? -1 : 1;
    white_last = last_white_slice_row(material);
    black_first = material->black_men == 0 ? -1 : 8;
    black_last = last_black_slice_row(material);
    for (int white = white_first;; ++white) {
        for (int black = black_first;; --black) {
            int ws = white < 0 ? 0 : white;
            int bs = black < 0 ? 0 : black;
            if (!completed[ws][bs]) {
                EgtbGenerationStatistics generated = {0};
                if (!generate_one_slice(directory, material, white, black,
                                        options, contexts, probe_contexts,
                                        &generated))
                    goto done;
                slice_statistics[ws][bs] = generated;
                completed[ws][bs] = true;
                if (!write_manifest(directory, material,
                                    eg_position_count(full_indexer),
                                    options->page_size, completed,
                                    slice_statistics))
                    goto done;
            } else if (!options->quiet) {
                egtb_progress_log("resuming: slice white-row=%d black-row=%d already complete\n",
                       white < 0 ? 0 : white + 1,
                       black < 0 ? 0 : black + 1);
            }
            add_generation_statistics(&total, &slice_statistics[ws][bs]);
            if (black == black_last)
                break;
        }
        if (white == white_last)
            break;
    }
    if (!options->quiet) {
        egtb_progress_log("compiling completed slices into %s\n", path);
        fflush(stdout);
    }
    if (!compile_slices(out, path, directory, material, full_indexer, options))
        goto done;
    if (statistics != NULL)
        *statistics = total;
    ok = true;
done:
    if (contexts != NULL)
        for (unsigned i = 0; i < options->thread_count; ++i)
            close_probe_context(&contexts[i]);
    free(probe_contexts);
    free(contexts);
    return ok;
}

bool egtb_sliced_cleanup(const char *path, const EgtbMaterial *material)
{
    char directory[512], slice_path[512], incomplete[512], manifest[512];
    int white_first, white_last, black_first, black_last;
    bool ok = true;
    if (path == NULL || material == NULL ||
        !make_work_directory(directory, sizeof(directory), path))
        return sliced_fail("invalid sliced cleanup request");
    white_first = material->white_men == 0 ? -1 : 1;
    white_last = last_white_slice_row(material);
    black_first = material->black_men == 0 ? -1 : 8;
    black_last = last_black_slice_row(material);
    for (int white = white_first;; ++white) {
        for (int black = black_first;; --black) {
            if (make_slice_path(slice_path, sizeof(slice_path), directory,
                                white, black, "") &&
                unlink(slice_path) != 0 && errno != ENOENT)
                ok = false;
            if (make_slice_path(incomplete, sizeof(incomplete), directory,
                                white, black, ".incomplete") &&
                unlink(incomplete) != 0 && errno != ENOENT)
                ok = false;
            if (black == black_last)
                break;
        }
        if (white == white_last)
            break;
    }
    snprintf(manifest, sizeof(manifest), "%s/manifest", directory);
    if (unlink(manifest) != 0 && errno != ENOENT)
        ok = false;
    snprintf(manifest, sizeof(manifest), "%s/manifest.incomplete", directory);
    if (unlink(manifest) != 0 && errno != ENOENT)
        ok = false;
    if (rmdir(directory) != 0 && errno != ENOENT)
        ok = false;
    return ok ? true : sliced_fail("cannot completely remove slice workspace");
}
