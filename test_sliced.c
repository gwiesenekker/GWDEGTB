#define _POSIX_C_SOURCE 200809L

#include "sliced.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool draw_probe(const DraughtsPosition *position, EgtbSide side,
                       void *context, int16_t *value)
{
    (void)position;
    (void)side;
    (void)context;
    *value = EGTB_DRAW;
    return true;
}

int main(int argc, char **argv)
{
    char directory[] = "/tmp/gwdegtb-sliced-test-XXXXXX";
    char unsliced_path[256] = "", sliced_path[256] = "";
    char manifest_path[256] = "";
    EgIndexer indexer = {0};
    Egtb *unsliced = NULL, *sliced = NULL;
    Bitmap verified = {0};
    EgtbGenerationStatistics unsliced_statistics = {0};
    EgtbGenerationStatistics sliced_statistics = {0};
    uint64_t first_sliced_passes;
    EgtbMaterial material = {0, 1, 0, 1};
    EgtbCreateOptions create_options = {64, 20, 3};
    EgtbThreadOptions thread_options = {2, 64, NULL, &verified, 4096};
    EgtbSlicedOptions sliced_options = {
        2, 1024, 64, 4, 64, 20, 3,
        draw_probe, NULL, NULL, NULL, NULL, true, 4096, 0, false
    };
    bool ok = false;
    uint32_t page_size = 1024;
    if (argc >= 2 && strcmp(argv[1], "2048") == 0)
        page_size = 2048;
    else if (argc >= 2 && strcmp(argv[1], "1024") != 0)
        return EXIT_FAILURE;
    if (argc >= 3 && strcmp(argv[2], "resident") == 0)
        sliced_options.resident_limit_bytes = 1048576;
    else if (argc >= 3 && strcmp(argv[2], "cached") != 0)
        return EXIT_FAILURE;
    if (argc == 4 && strcmp(argv[3], "nine") == 0)
        material = (EgtbMaterial){1, 0, 0, 1};
    else if (argc == 4 && strcmp(argv[3], "wide-nine") == 0)
        material = (EgtbMaterial){2, 0, 0, 1};
    else if (argc == 4 && strcmp(argv[3], "wide-81") == 0)
        material = (EgtbMaterial){1, 1, 0, 1};
    else if (argc == 4 && strcmp(argv[3], "benchmark") == 0)
        material = (EgtbMaterial){1, 1, 1, 1};
    else if (argc > 3)
        return EXIT_FAILURE;
    sliced_options.page_size = page_size;
    if (mkdtemp(directory) == NULL)
        goto done;
    snprintf(unsliced_path, sizeof(unsliced_path), "%s/unsliced.dtm",
             directory);
    snprintf(sliced_path, sizeof(sliced_path), "%s/sliced.dtm", directory);
    snprintf(manifest_path, sizeof(manifest_path), "%s.work/manifest",
             sliced_path);
    if (!eg_indexer_init(&indexer, material.white_men, material.black_men,
                         material.white_kings, material.black_kings) ||
        !egtb_create(&unsliced, unsliced_path,
                     eg_position_count(&indexer) - 1, page_size,
                     &create_options) ||
        !egtb_generate_threaded(unsliced, &indexer, draw_probe, NULL,
                                NULL, NULL, &thread_options,
                                &unsliced_statistics) ||
        !egtb_generate_sliced(&sliced, sliced_path, &material, &indexer,
                              &sliced_options, &sliced_statistics))
        goto done;
    if (unsliced_statistics.consistency_updates[0] != 0 ||
        unsliced_statistics.consistency_updates[1] != 0 ||
        sliced_statistics.consistency_updates[0] != 0 ||
        sliced_statistics.consistency_updates[1] != 0)
        goto done;
    for (uint64_t index = 0; index < eg_position_count(&indexer); ++index)
        for (unsigned side = 0; side < 2; ++side) {
            int16_t expected = 0, actual = 0;
            if (!egtb_get(unsliced, index, (EgtbSide)side, &expected) ||
                !egtb_get(sliced, index, (EgtbSide)side, &actual) ||
                expected != actual) {
                fprintf(stderr,
                        "sliced mismatch at index %llu side %u: %d != %d\n",
                        (unsigned long long)index, side, actual, expected);
                goto done;
            }
        }
    first_sliced_passes = sliced_statistics.retrograde_passes;
    if (!egtb_close(sliced))
        goto done;
    sliced = NULL;
    if (unlink(sliced_path) != 0)
        goto done;
    {
        FILE *manifest = fopen(manifest_path, "r+b");
        long corruption_offset = -1;
        int original = EOF;
        char line[256];
        if (manifest == NULL)
            goto done;
        while (fgets(line, sizeof(line), manifest) != NULL)
            if (strncmp(line, "revision ", 9) == 0) {
                corruption_offset = ftell(manifest) - (long)strlen(line) + 9;
                break;
            }
        if (corruption_offset < 0 ||
            fseek(manifest, corruption_offset, SEEK_SET) != 0 ||
            (original = fgetc(manifest)) == EOF ||
            fseek(manifest, corruption_offset, SEEK_SET) != 0 ||
            fputc(original == '9' ? '8' : '9', manifest) == EOF ||
            fclose(manifest) != 0)
            goto done;
        manifest = NULL;
        if (egtb_generate_sliced(&sliced, sliced_path, &material, &indexer,
                                 &sliced_options, &sliced_statistics) ||
            sliced != NULL ||
            strstr(egtb_sliced_last_error(), "checksum mismatch") == NULL)
            goto done;
        manifest = fopen(manifest_path, "r+b");
        if (manifest == NULL ||
            fseek(manifest, corruption_offset, SEEK_SET) != 0 ||
            fputc(original, manifest) == EOF || fclose(manifest) != 0)
            goto done;
    }
    const unsigned merge_threads[] = {1, 2, 4, 8, 16};
    for (unsigned run = 0; run < sizeof(merge_threads) / sizeof(merge_threads[0]); ++run) {
        sliced_options.thread_count = merge_threads[run];
        memset(&sliced_statistics, 0, sizeof(sliced_statistics));
        if (!egtb_generate_sliced(&sliced, sliced_path, &material, &indexer,
                                  &sliced_options, &sliced_statistics) ||
            sliced_statistics.retrograde_passes != first_sliced_passes ||
            sliced_statistics.resumed_slices == 0 ||
            sliced_statistics.initialization_seconds != 0 ||
            sliced_statistics.backpropagation_seconds != 0 ||
            sliced_statistics.compilation_seconds != 0 ||
            sliced_statistics.consistency_seconds != 0)
            goto done;
        for (uint64_t index = 0; index < eg_position_count(&indexer); ++index)
            for (unsigned side = 0; side < 2; ++side) {
                int16_t expected = 0, actual = 0;
                if (!egtb_get(unsliced, index, (EgtbSide)side, &expected) ||
                    !egtb_get(sliced, index, (EgtbSide)side, &actual) ||
                    expected != actual)
                    goto done;
            }
        printf("sliced merge: threads=%u seconds=%.6f\n", merge_threads[run],
               sliced_statistics.slice_merge_seconds);
        if (!egtb_close(sliced)) goto done;
        sliced = NULL;
        if (unlink(sliced_path) != 0) goto done;
    }
    /* Truncate a completed slice: the parallel resume scan must reject it and
     * regenerate it, not blindly trust the checksummed manifest. */
    {
        char damaged[512];
        snprintf(damaged, sizeof(damaged), "%s.work/slice-w%02d-b09.dtm",
                 sliced_path, material.white_men ? 2 : 0);
        FILE *f = fopen(damaged, "r+b");
        if (!f || fseek(f, 0, SEEK_END) != 0) goto done;
        long size = ftell(f);
        if (fclose(f) != 0 || size <= 64 || truncate(damaged, size - 1) != 0) goto done;
        memset(&sliced_statistics, 0, sizeof(sliced_statistics));
        if (!egtb_generate_sliced(&sliced, sliced_path, &material, &indexer,
                                  &sliced_options, &sliced_statistics) ||
            sliced_statistics.initialization_seconds <= 0) goto done;
        for (uint64_t i=0; i<eg_position_count(&indexer); ++i)
            for (unsigned side=0; side<2; ++side) {
                int16_t a, b;
                if (!egtb_get(unsliced,i,(EgtbSide)side,&a) ||
                    !egtb_get(sliced,i,(EgtbSide)side,&b) || a != b) goto done;
            }
    }
    printf("sliced generation regression: PASS (%llu positions, %llu passes)\n",
           (unsigned long long)eg_position_count(&indexer),
           (unsigned long long)sliced_statistics.retrograde_passes);
    ok = true;
done:
    if (unsliced != NULL)
        egtb_close(unsliced);
    if (sliced != NULL)
        egtb_close(sliced);
    bitmap_destroy(&verified);
    eg_indexer_destroy(&indexer);
    if (sliced_path[0] != '\0') {
        egtb_sliced_cleanup(sliced_path, &material);
        unlink(sliced_path);
    }
    if (unsliced_path[0] != '\0')
        unlink(unsliced_path);
    if (sliced_path[0] != '\0')
        rmdir(directory);
    if (!ok)
        fprintf(stderr, "sliced generation regression failed: %s / %s / %s\n",
                egtb_sliced_last_error(), egtb_generator_last_error(),
                egtb_last_error());
    return ok ? 0 : 1;
}
