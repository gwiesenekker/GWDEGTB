#include "wdl.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *stream)
{
    fprintf(stream, "Usage: dtm_to_wdl [-j threads] [-l level] [-o output.wdl] [--force] input.dtm\n"
                    "  -j threads   conversion workers (default EGTB_WDL_THREADS, otherwise 4)\n"
                    "  -l level     Zstd compression level (default 12)\n"
                    "  -o path      output path (default input path with .wdl extension)\n"
                    "  --force      replace an existing WDL file\n");
}

static bool parse_number(const char *value, unsigned maximum, unsigned *result)
{
    char *end;
    unsigned long number;
    if (value == NULL || *value == '\0' || *value == '-') return false;
    errno = 0;
    number = strtoul(value, &end, 10);
    if (errno != 0 || *end != '\0' || number < 1 || number > maximum)
        return false;
    *result = (unsigned)number;
    return true;
}

int main(int argc, char **argv)
{
    const char *input = NULL, *output = NULL;
    char *derived = NULL;
    unsigned threads = wdl_default_threads(), level = 12;
    bool force = false;
    WdlStatistics statistics;
    WdlStorageStatistics storage;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(stdout);
            return 0;
        }
        if (!strcmp(argv[i], "--force")) {
            force = true;
        } else if ((!strcmp(argv[i], "-j") || !strcmp(argv[i], "-l") ||
                    !strcmp(argv[i], "-o")) && i + 1 < argc) {
            const char *option = argv[i++];
            if (!strcmp(option, "-o")) output = argv[i];
            else if (!parse_number(argv[i], !strcmp(option, "-j") ?
                                   WDL_MAX_DECOMPRESSION_THREADS :
                                   (unsigned)ZSTD_maxCLevel(),
                                   !strcmp(option, "-j") ? &threads : &level)) {
                fprintf(stderr, "Invalid %s value: %s\n", option, argv[i]);
                return 2;
            }
        } else if (argv[i][0] == '-' || input != NULL) {
            usage(stderr);
            return 2;
        } else input = argv[i];
    }
    if (input == NULL || threads == 0 || (output != NULL && *output == '\0')) {
        usage(stderr);
        return 2;
    }
    size_t length = strlen(input);
    if (length < 5 || strcmp(input + length - 4, ".dtm")) {
        fprintf(stderr, "Input must have a .dtm extension: %s\n", input);
        return 2;
    }
    if (output == NULL) {
        derived = malloc(length + 1);
        if (derived == NULL) {
            fprintf(stderr, "Out of memory\n");
            return 1;
        }
        memcpy(derived, input, length + 1);
        memcpy(derived + length - 4, ".wdl", 4);
        output = derived;
    }
    if (!strcmp(input, output)) {
        fprintf(stderr, "Input and output paths must differ\n");
        free(derived);
        return 2;
    }
    errno = 0;
    FILE *existing = fopen(output, "rb");
    if (existing != NULL) {
        fclose(existing);
        if (!force) {
            printf("WDL already exists: %s (use --force to replace)\n", output);
            free(derived);
            return 0;
        }
    } else if (errno != ENOENT) {
        fprintf(stderr, "Cannot inspect output %s: %s\n", output, strerror(errno));
        free(derived);
        return 1;
    }
    printf("Converting %s to %s with %u threads (Zstd level %u)\n",
           input, output, threads, level);
    fflush(stdout);
    bool ok = wdl_compile_threaded(input, output, (int)level, 2, threads,
                                   &statistics, &storage);
    if (!ok) {
        fprintf(stderr, "WDL conversion failed: %s\n", wdl_last_error());
        free(derived);
        return 1;
    }
    printf("WDL generated: %s raw=%" PRIu64 " bytes file=%" PRIu64
           " bytes overall=%.2f%% (%.2f:1)\n",
           output, storage.logical_uncompressed_bytes, storage.file_bytes,
           100.0 * (double)storage.file_bytes /
               (double)storage.logical_uncompressed_bytes,
           (double)storage.logical_uncompressed_bytes / (double)storage.file_bytes);
    printf("WDL WTM: wins=%" PRIu64 " draws=%" PRIu64 " losses=%" PRIu64
           "\nWDL BTM: wins=%" PRIu64 " draws=%" PRIu64 " losses=%" PRIu64 "\n",
           statistics.wins[EGTB_WHITE_TO_MOVE], statistics.draws[EGTB_WHITE_TO_MOVE],
           statistics.losses[EGTB_WHITE_TO_MOVE], statistics.wins[EGTB_BLACK_TO_MOVE],
           statistics.draws[EGTB_BLACK_TO_MOVE], statistics.losses[EGTB_BLACK_TO_MOVE]);
    free(derived);
    return 0;
}
