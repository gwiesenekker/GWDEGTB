#include "endgame_index.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

static bool parse_count(const char *text, unsigned *value)
{
    char *end;
    unsigned long parsed;
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || *text == '\0' || *end != '\0' || parsed > 50)
        return false;
    *value = (unsigned)parsed;
    return true;
}

int main(int argc, char **argv)
{
    unsigned material[4] = {1, 1, 0, 0};
    EgIndexer indexer;
    uint64_t tested;
    int i;

    if (argc != 1 && argc != 5) {
        fprintf(stderr, "usage: %s [white-men black-men white-kings black-kings]\n",
                argv[0]);
        return EXIT_FAILURE;
    }
    for (i = 1; i < argc; ++i) {
        if (!parse_count(argv[i], &material[i - 1])) {
            fprintf(stderr, "invalid piece count: %s\n", argv[i]);
            return EXIT_FAILURE;
        }
    }
    if (!eg_indexer_init(&indexer, material[0], material[1],
                         material[2], material[3])) {
        fprintf(stderr, "material is impossible or its index does not fit in uint64_t\n");
        return EXIT_FAILURE;
    }
    printf("material: WM=%u BM=%u WK=%u BK=%u\n", material[0], material[1],
           material[2], material[3]);
    printf("legal positions: %" PRIu64 "\n", eg_position_count(&indexer));
    printf("maximum index:  %" PRIu64 "\n", eg_max_index(&indexer));
    eg_indexer_destroy(&indexer);

    if (!eg_test_material(material[0], material[1], material[2], material[3],
                          &tested)) {
        fprintf(stderr, "round-trip test FAILED after %" PRIu64 " positions\n",
                tested);
        return EXIT_FAILURE;
    }
    printf("round-trip test: PASS (%" PRIu64 " positions)\n", tested);
    return EXIT_SUCCESS;
}
