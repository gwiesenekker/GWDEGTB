/* Read-only full scan: deliberately never calls an auto-generating WDL API. */
#include "gwdegtb.h"
#include "wdl.h"
#include "endgame_index.h"
#include "material.h"
#include "compat.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    Egtb *dtm;
    WdlImage *image;
    const char *name;
    uint64_t count, first_page, end_page;
    uint64_t histogram[2][65536];
    uint64_t totals[2][3];
    int status;
} ScanWorker;

static void *scan_worker(void *argument)
{
    ScanWorker *worker = argument;
    Egtb *dtm = worker->dtm;
    WdlImage *image = worker->image;
    const char *name = worker->name;
    uint64_t count = worker->count;
    EgtbView *view = NULL;
    EgtbSequentialReader reader;
    ZSTD_DCtx *decoder = ZSTD_createDCtx();
    int status = 1;
    if (!decoder) {
        fprintf(stderr, "Cannot allocate scan buffers\n");
        goto done;
    }
    uint64_t end = worker->end_page * (WDL_PAGE_SIZE * 2);
    if (end > count) end = count;
    if (!egtb_view_create(&view, dtm, 4, false) ||
        !egtb_sequential_reader_init(&reader, view,
            worker->first_page * (WDL_PAGE_SIZE * 2), end)) {
        fprintf(stderr, "%s\n", egtb_last_error());
        goto done;
    }
    for (uint64_t page = worker->first_page; page < worker->end_page; ++page) {
        unsigned char unpacked[WDL_PAGE_SIZE];
        const void *compressed;
        size_t bytes;
        uint32_t checksum;
        if (!wdl_image_page(image, page, &compressed, &bytes, &checksum)) {
            fprintf(stderr, "%s\n", wdl_last_error());
            goto done;
        }
        if (!compressed) memset(unpacked, 0, sizeof(unpacked));
        else {
            size_t size = ZSTD_decompress_usingDDict(decoder, unpacked,
                sizeof(unpacked), compressed, bytes, wdl_image_dictionary(image));
            if (ZSTD_isError(size) || size != sizeof(unpacked)) {
                fprintf(stderr, "WDL page %" PRIu64 ": invalid compressed data\n", page);
                goto done;
            }
        }
        if (!wdl_image_validate_page(image, page, unpacked, sizeof(unpacked))) {
            fprintf(stderr, "%s\n", wdl_last_error());
            goto done;
        }
        for (unsigned offset = 0; offset < WDL_PAGE_SIZE * 2; ++offset) {
            uint64_t index = page * (WDL_PAGE_SIZE * 2) + offset;
            if (index >= count) break;
            int16_t value[2];
            if (!egtb_sequential_reader_next(&reader, &value[0], &value[1])) {
                fprintf(stderr, "DTM index %" PRIu64 ": %s\n", index, egtb_last_error());
                goto done;
            }
            for (unsigned side = 0; side < 2; ++side) {
                int v = value[side];
                unsigned actual = (unpacked[offset / 2] >> ((offset % 2) * 4 + side * 2)) & 3;
                unsigned expected = v == -1 ? WDL_DRAW : v > 0 ? WDL_WIN : WDL_LOSS;
                if (v == INT16_MIN || (v > 0 && !(v & 1)) ||
                    (v < -1 && v % 2 != 0) || actual != expected) {
                    fprintf(stderr, "%s index=%" PRIu64 " %s: invalid or mismatched DTM=%d WDL-code=%u\n",
                        name, index, side ? "BTM" : "WTM", v, actual);
                    goto done;
                }
                ++worker->histogram[side][v + 32768];
                ++worker->totals[side][actual];
            }
        }
    }
    status = 0;
done:
    if (view && !egtb_view_close(view)) status = 1;
    ZSTD_freeDCtx(decoder);
    worker->status = status;
    return NULL;
}

static int scan(Egtb *dtm, WdlImage *image, const char *name, uint64_t count,
                unsigned threads)
{
    uint64_t pages = wdl_image_page_count(image);
    if (pages < threads) threads = (unsigned)pages;
    if (!threads) return 1;
    ScanWorker *workers = calloc(threads, sizeof(*workers));
    my_thread_t *handles = calloc(threads, sizeof(*handles));
    unsigned started = 0;
    int status = 1;
    if (!workers || !handles) {
        fprintf(stderr, "Cannot allocate worker buffers\n");
        goto done;
    }
    printf("Scanning %s: %" PRIu64 " positions, both sides, %u scanning threads\n",
           name, count, threads);
    fflush(stdout);
    for (unsigned i = 0; i < threads; ++i) {
        workers[i].dtm = dtm;
        workers[i].image = image;
        workers[i].name = name;
        workers[i].count = count;
        workers[i].first_page = pages / threads * i + (pages % threads * i) / threads;
        workers[i].end_page = pages / threads * (i + 1) + (pages % threads * (i + 1)) / threads;
        workers[i].status = 1;
        int error = compat_thread_create(&handles[i], scan_worker, &workers[i]);
        if (error) {
            fprintf(stderr, "Cannot start scan worker: %s\n", strerror(error));
            break;
        }
        ++started;
    }
    for (unsigned i = 0; i < started; ++i) {
        if (compat_thread_join(handles[i])) {
            /* Cannot safely release shared buffers if a worker may still run. */
            fprintf(stderr, "Cannot join scan worker\n");
            exit(EXIT_FAILURE);
        }
    }
    if (started != threads) goto done;
    for (unsigned i = 0; i < threads; ++i)
        if (workers[i].status) goto done;
    for (unsigned i = 1; i < threads; ++i)
        for (unsigned side = 0; side < 2; ++side) {
            for (unsigned v = 0; v < 65536; ++v)
                workers[0].histogram[side][v] += workers[i].histogram[side][v];
            for (unsigned v = 0; v < 3; ++v)
                workers[0].totals[side][v] += workers[i].totals[side][v];
        }
    uint64_t (*histogram)[65536] = workers[0].histogram;
    uint64_t (*totals)[3] = workers[0].totals;
    for (unsigned side = 0; side < 2; ++side) {
        if (totals[side][0] + totals[side][1] + totals[side][2] != count) {
            fprintf(stderr, "Incomplete scan\n");
            goto done;
        }
        printf("%s: wins=%" PRIu64 " losses=%" PRIu64 " draws=%" PRIu64 " (DTM and WDL agree)\n",
            side ? "BTM" : "WTM", totals[side][WDL_WIN], totals[side][WDL_LOSS], totals[side][WDL_DRAW]);
        printf("%s DTM statistics:\n     DTM            Frequency\n", side ? "BTM" : "WTM");
        for (int v = -32768; v <= 32767; ++v)
            if (histogram[side][v + 32768])
                printf("%8d %20" PRIu64 "\n", v, histogram[side][v + 32768]);
    }
    printf("Full scan passed: %" PRIu64 " DTM/WDL values checked. Not a move-based verification.\n", count * 2);
    status = 0;
done:
    free(handles);
    free(workers);
    return status;
}

static uint64_t padded(uint64_t bits)
{
    uint64_t result = 0;
    for (unsigned s = 0; s < 50; ++s)
        if (bits & (UINT64_C(1) << s))
            result |= UINT64_C(1) << (s + 6 + s / 10);
    return result;
}

static int check(const char *directory, const char *name, unsigned threads)
{
    unsigned wk, wm, bk, bm;
    int end = 0, status = 1;
    char path[4096];
    EgIndexer indexer = {0};
    Egtb *dtm = NULL;
    WdlImage *image = NULL;
    GwdegtbWdlProbe *probe = NULL;
    void *data = NULL;
    size_t bytes = 0;
    if (sscanf(name, "%uwX-%uwO-%ubX-%ubO%n", &wk, &wm, &bk, &bm, &end) != 4 ||
        name[end] || wk > 8 || wm > 8 || bk > 8 || bm > 8 ||
        wk + wm + bk + bm > 8 || ! (wk + wm) || !(bk + bm)) {
        fprintf(stderr, "Invalid basename: %s (omit .dtm/.wdl)\n", name);
        return 1;
    }
    EgtbMaterial requested = {wk, wm, bk, bm}, canonical;
    if (egtb_material_resolve(&requested, &canonical) != EGTB_MATERIAL_CANONICAL) {
        fprintf(stderr, "Use the canonical database basename: %s\n", name);
        return 1;
    }
#define CHECK(condition, message) do { if (!(condition)) { \
    fprintf(stderr, "%s: %s\n", name, message); goto done; } } while (0)
    CHECK(eg_indexer_init(&indexer, wm, bm, wk, bk), "indexer initialization failed");
    int n = snprintf(path, sizeof(path), "%s/%s.dtm", directory, name);
    CHECK(n > 0 && (size_t)n < sizeof(path), "path too long");
    CHECK(egtb_open_readonly(&dtm, path, 256), egtb_last_error());
    CHECK(egtb_maximum_index(dtm) == eg_max_index(&indexer), "DTM index size mismatch");
    n = snprintf(path, sizeof(path), "%s/%s.wdl", directory, name);
    CHECK(n > 0 && (size_t)n < sizeof(path), "path too long");
    CHECK(wdl_file_size(path, &bytes), wdl_last_error());
    CHECK(bytes != 0 && (data = malloc(bytes)) != NULL, "WDL allocation failed");
    CHECK(wdl_file_load_into_threaded(path, data, bytes, threads), wdl_last_error());
    CHECK(wdl_image_attach(&image, data, bytes), wdl_last_error());
    CHECK(wdl_image_maximum_index(image) == eg_max_index(&indexer), "WDL index size mismatch");
    CHECK(gwdegtb_wdl_compressed_attach(name, data, bytes), gwdegtb_last_error());
    CHECK(gwdegtb_wdl_probe_create(1024 * 1024, &probe), gwdegtb_last_error());
    uint64_t count = eg_position_count(&indexer);
    CHECK(scan(dtm, image, name, count, threads) == 0, "full scan failed");
    uint64_t samples = count < 10000 ? count : 10000;
    for (uint64_t i = 0; i < samples; ++i) {
        /* Include both endpoints; avoid multiplication overflow. */
        uint64_t index = samples == 1 ? 0 :
            ((count - 1) / (samples - 1)) * i +
            ((count - 1) % (samples - 1)) * i / (samples - 1);
        EgPosition p;
        CHECK(eg_index_to_position(&indexer, index, &p), "index inversion failed");
        for (unsigned side = 0; side < 2; ++side) {
            int16_t value;
            CHECK(egtb_get(dtm, index, (EgtbSide)side, &value), egtb_last_error());
            int16_t expected = value == -1 ? -1 : value > 0 ? 1 : 0;
            int16_t actual = gwdegtb_wdl_lookup_probe_compact(probe,
                p.white_kings, p.white_men, p.black_kings, p.black_men, (GwdegtbSide)side);
            int16_t via_api = gwdegtb_dtm_lookup_compact(directory, 1024 * 1024,
                p.white_kings, p.white_men, p.black_kings, p.black_men, (GwdegtbSide)side);
            int16_t via_padded = gwdegtb_wdl_lookup_probe(probe,
                padded(p.white_kings), padded(p.white_men),
                padded(p.black_kings), padded(p.black_men), (GwdegtbSide)side);
            if (value == GWDEGTB_DTM_UNAVAILABLE || actual != expected ||
                via_padded != expected || via_api != value) {
                fprintf(stderr, "%s index=%" PRIu64 " side=%u: DTM=%d API=%d WDL=%d padded=%d\n",
                        name, index, side, value, via_api, actual, via_padded);
                goto done;
            }
        }
    }
    printf("PASS %s: additional API checks at %" PRIu64 " positions, both sides; WDL=%zu bytes, %u loading threads\n",
           name, samples, bytes, threads);
    status = 0;
done:
    gwdegtb_wdl_probe_destroy(probe);
    gwdegtb_wdl_compressed_unload_all();
    gwdegtb_dtm_close_all();
    wdl_image_destroy(image);
    free(data);
    if (dtm && !egtb_close(dtm)) status = 1;
    eg_indexer_destroy(&indexer);
    return status;
#undef CHECK
}

int main(int argc, char **argv)
{
    unsigned threads = 4;
    int first = 1;
    if (argc > 1 && !strcmp(argv[1], "-j")) {
        char *end;
        if (argc < 3 || argv[2][0] < '0' || argv[2][0] > '9') goto usage;
        errno = 0;
        unsigned long value = strtoul(argv[2], &end, 10);
        if (errno || *end || value < 1 || value > 256) goto usage;
        threads = (unsigned)value;
        first = 3;
    }
    if (argc - first < 2) {
usage:
        fprintf(stderr, "Usage: %s [-j THREADS] DIRECTORY BASENAME [BASENAME ...]\n"
                "THREADS: 1..256, default 4; controls loading and full scanning.\n"
                "Reads existing .dtm and .wdl pairs only; never creates files.\n", argv[0]);
        return 2;
    }
    int result = 0;
    for (int i = first + 1; i < argc; ++i) result |= check(argv[first], argv[i], threads);
    return result;
}
