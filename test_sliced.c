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
        draw_probe, NULL, NULL, NULL, NULL, true, 4096
    };
    bool ok = false;
    uint32_t page_size = 1024;
    if (argc == 2 && strcmp(argv[1], "2048") == 0)
        page_size = 2048;
    else if (argc != 1)
        return EXIT_FAILURE;
    sliced_options.page_size = page_size;
    if (mkdtemp(directory) == NULL)
        goto done;
    snprintf(unsliced_path, sizeof(unsliced_path), "%s/unsliced.dtm",
             directory);
    snprintf(sliced_path, sizeof(sliced_path), "%s/sliced.dtm", directory);
    if (!eg_indexer_init(&indexer, 1, 1, 0, 0) ||
        !egtb_create(&unsliced, unsliced_path,
                     eg_position_count(&indexer) - 1, page_size,
                     &create_options) ||
        !egtb_generate_threaded(unsliced, &indexer, draw_probe, NULL,
                                NULL, NULL, &thread_options,
                                &unsliced_statistics) ||
        !egtb_generate_sliced(&sliced, sliced_path, &material, &indexer,
                              &sliced_options, &sliced_statistics))
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
    memset(&sliced_statistics, 0, sizeof(sliced_statistics));
    if (!egtb_generate_sliced(&sliced, sliced_path, &material, &indexer,
                              &sliced_options, &sliced_statistics) ||
        sliced_statistics.retrograde_passes != first_sliced_passes)
        goto done;
    for (uint64_t index = 0; index < eg_position_count(&indexer); ++index)
        for (unsigned side = 0; side < 2; ++side) {
            int16_t expected = 0, actual = 0;
            if (!egtb_get(unsliced, index, (EgtbSide)side, &expected) ||
                !egtb_get(sliced, index, (EgtbSide)side, &actual) ||
                expected != actual)
                goto done;
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
