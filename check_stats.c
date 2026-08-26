#include "endgame_index.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    const char *path = argc == 2 ? argv[1] : "7piece-stats.txt";
    FILE *input;
    char line[256];
    unsigned line_number = 0;
    unsigned checked = 0;
    unsigned mismatches = 0;

    if (argc > 2) {
        fprintf(stderr, "usage: %s [statistics-file]\n", argv[0]);
        return EXIT_FAILURE;
    }
    input = fopen(path, "r");
    if (input == NULL) {
        perror(path);
        return EXIT_FAILURE;
    }

    while (fgets(line, sizeof(line), input) != NULL) {
        unsigned wm, wk, bm, bk;
        uint64_t expected_count, expected_max;
        int consumed = 0;
        const char *tail;
        EgIndexer indexer;

        ++line_number;
        for (tail = line; isspace((unsigned char)*tail); ++tail)
            ;
        if (*tail == '\0')
            continue;
        if (sscanf(line,
                   "wm=%u wk=%u bm=%u bk=%u positions=%" SCNu64
                   " max-index=%" SCNu64 " %n",
                   &wm, &wk, &bm, &bk, &expected_count, &expected_max,
                   &consumed) != 6 || line[consumed] != '\0') {
            fprintf(stderr, "%s:%u: malformed statistics line\n", path,
                    line_number);
            ++mismatches;
            continue;
        }
        if (!eg_indexer_init(&indexer, wm, bm, wk, bk)) {
            fprintf(stderr,
                    "%s:%u: cannot construct WM=%u WK=%u BM=%u BK=%u\n",
                    path, line_number, wm, wk, bm, bk);
            ++mismatches;
            continue;
        }
        ++checked;
        if (eg_position_count(&indexer) != expected_count ||
            eg_max_index(&indexer) != expected_max) {
            printf("MISMATCH WM=%u WK=%u BM=%u BK=%u: "
                   "expected positions=%" PRIu64 " max-index=%" PRIu64
                   ", got positions=%" PRIu64 " max-index=%" PRIu64 "\n",
                   wm, wk, bm, bk, expected_count, expected_max,
                   eg_position_count(&indexer), eg_max_index(&indexer));
            ++mismatches;
        }
        eg_indexer_destroy(&indexer);
    }
    if (ferror(input)) {
        perror(path);
        fclose(input);
        return EXIT_FAILURE;
    }
    fclose(input);

    if (mismatches != 0) {
        printf("FAILED: %u entries checked, %u problem(s) found\n", checked,
               mismatches);
        return EXIT_FAILURE;
    }
    printf("PASS: all %u entries match\n", checked);
    return EXIT_SUCCESS;
}
