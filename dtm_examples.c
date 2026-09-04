#define _POSIX_C_SOURCE 200809L

#include "dtm_fen.h"
#include "egtb.h"
#include "endgame_index.h"
#include "material.h"
#include "progress.h"
#include "revision.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool parse_count(const char *text, unsigned *count)
{
    char *end;
    unsigned long value;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || *text == '\0' || *end != '\0' ||
        value > EGTB_MAX_PIECES)
        return false;
    *count = (unsigned)value;
    return true;
}

static void print_example(const char *label, const EgIndexer *indexer,
                          const EgtbDtmExample *example, EgtbSide side)
{
    char fen[256];
    if (example->available &&
        egtb_format_dtm_fen(indexer, example->index, side, example->dtm,
                            fen, sizeof(fen)))
        printf("%-20s %s\n", label, fen);
    else
        printf("%-20s unavailable\n", label);
}

int main(int argc, char **argv)
{
    const char *directory = ".";
    int argument = 1;
    EgtbMaterial requested, material;
    EgtbMaterialKind kind;
    EgIndexer indexer;
    Egtb *database = NULL;
    EgtbDtmExamples examples;
    char filename[96], path[4096];
    bool indexer_initialized = false;
    bool ok = false;

    if (argc == 2 && strcmp(argv[1], "--revision") == 0) {
        printf("GWDEGTB revision %s\n", gwdegtb_revision);
        return EXIT_SUCCESS;
    }
    if (argument + 1 < argc && strcmp(argv[argument], "-d") == 0) {
        directory = argv[argument + 1];
        argument += 2;
    }
    if (argc != argument + 4 ||
        !parse_count(argv[argument], &requested.white_kings) ||
        !parse_count(argv[argument + 1], &requested.white_men) ||
        !parse_count(argv[argument + 2], &requested.black_kings) ||
        !parse_count(argv[argument + 3], &requested.black_men)) {
        fprintf(stderr, "usage: %s [-d DIRECTORY] NWHITE_KINGS NWHITE_MEN "
                        "NBLACK_KINGS NBLACK_MEN\n", argv[0]);
        return EXIT_FAILURE;
    }
    kind = egtb_material_resolve(&requested, &material);
    if (kind != EGTB_MATERIAL_CANONICAL && kind != EGTB_MATERIAL_MIRROR) {
        fprintf(stderr, "material must contain 2..%u pieces and at least "
                        "one piece for each side\n", EGTB_MAX_PIECES);
        return EXIT_FAILURE;
    }
    if (!egtb_material_filename(filename, sizeof(filename),
                                material.white_kings, material.white_men,
                                material.black_kings, material.black_men,
                                "dtm") ||
        snprintf(path, sizeof(path), "%s%s%s", directory,
                 *directory != '\0' && directory[strlen(directory) - 1] != '/'
                     ? "/" : "",
                 filename) >= (int)sizeof(path)) {
        fprintf(stderr, "database path is too long\n");
        goto done;
    }
    if (kind == EGTB_MATERIAL_MIRROR)
        printf("requested material is mirrored; querying canonical %s\n",
               filename);
    if (!eg_indexer_init(&indexer, material.white_men, material.black_men,
                         material.white_kings, material.black_kings)) {
        fprintf(stderr, "cannot initialize material indexer\n");
        goto done;
    }
    indexer_initialized = true;
    if (!egtb_open_readonly(&database, path, 1)) {
        fprintf(stderr, "cannot open %s: %s\n", path, egtb_last_error());
        goto done;
    }
    if (egtb_maximum_index(database) != eg_max_index(&indexer)) {
        fprintf(stderr, "%s maximum index does not match its material\n",
                path);
        goto done;
    }
    if (!egtb_progress_start(60)) {
        fprintf(stderr, "cannot start progress reporter\n");
        goto done;
    }
    egtb_progress_log("GWDEGTB revision %s scanning %s\n",
                      gwdegtb_revision, path);
    egtb_progress_begin("DTM examples", eg_position_count(&indexer),
                        "positions");
    if (!egtb_find_dtm_examples(database, NULL, &examples)) {
        egtb_progress_end(false);
        fprintf(stderr, "cannot scan %s: %s\n", path, egtb_last_error());
        goto done;
    }
    egtb_progress_end(true);
    printf("DTM example positions for %s:\n", filename);
    print_example("WTM longest win", &indexer,
                  &examples.longest_win[EGTB_WHITE_TO_MOVE],
                  EGTB_WHITE_TO_MOVE);
    print_example("WTM longest loss", &indexer,
                  &examples.longest_loss[EGTB_WHITE_TO_MOVE],
                  EGTB_WHITE_TO_MOVE);
    print_example("BTM longest win", &indexer,
                  &examples.longest_win[EGTB_BLACK_TO_MOVE],
                  EGTB_BLACK_TO_MOVE);
    print_example("BTM longest loss", &indexer,
                  &examples.longest_loss[EGTB_BLACK_TO_MOVE],
                  EGTB_BLACK_TO_MOVE);
    print_example("draw", &indexer, &examples.draw, examples.draw_side);
    ok = true;

done:
    egtb_progress_stop();
    if (database != NULL && !egtb_close(database))
        ok = false;
    if (indexer_initialized)
        eg_indexer_destroy(&indexer);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
