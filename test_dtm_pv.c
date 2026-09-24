#include "dtm_pv.h"
#include "endgame_index.h"
#include "egtb.h"
#include "compat.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define remove_directory _rmdir
#else
#include <unistd.h>
#define remove_directory rmdir
#endif
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); goto done; } } while (0)
static bool run(const char *dir, const char *fen, bool success, const char *text)
{
    char error[512], buffer[4096];
    FILE *out = tmpfile();
    if (!out) return false;
    bool ok = gwdegtb_dtm_pv(dir, fen, out, error, sizeof error);
    rewind(out);
    size_t n = fread(buffer, 1, sizeof buffer - 1, out);
    buffer[n] = 0; fclose(out);
    size_t column = 0;
    for (size_t i = 0; i < n; ++i) {
        if (buffer[i] == '\n') column = 0;
        else if (++column > 80) return false;
    }
    if (success && (strstr(buffer, "W:") || strstr(buffer, "B:"))) return false;
    if (ok != success || !strstr(success ? buffer : error, text)) {
        fprintf(stderr, "FEN %s: %s\n%s", fen, error, buffer); return false;
    }
    return true;
}
int main(void)
{
    char dir[] = "pv-test-XXXXXX", path[128] = "";
    EgIndexer indexer = {0}; Egtb *db = NULL;
    int result = 1;
    int fd = compat_mkstemp(dir);
    if (fd < 0) return 1;
    compat_close(fd); compat_unlink(dir);
    if (compat_mkdir(dir)) return 1;
    snprintf(path, sizeof path, "%s/1wX-0wO-0bX-1bO.dtm", dir);
    CHECK(eg_indexer_init(&indexer, 0, 1, 1, 0));
    EgtbCreateOptions options = {8, 20, 1};
    CHECK(egtb_create(&db, path, eg_max_index(&indexer), 2048, &options));
    EgPosition p = {0, UINT64_C(1)<<42, UINT64_C(1)<<48, 0};
    uint64_t index;
    CHECK(eg_position_to_index(&indexer, &p, &index));
    CHECK(egtb_set(db, index, EGTB_WHITE_TO_MOVE, 1));
    EgPosition forced = {0, UINT64_C(1)<<40, UINT64_C(1)<<46, 0};
    uint64_t forced_index;
    CHECK(eg_position_to_index(&indexer, &forced, &forced_index));
    CHECK(egtb_set(db, forced_index, EGTB_WHITE_TO_MOVE, 1));
    CHECK(egtb_close(db)); db = NULL;
    CHECK(egtb_open_readonly(&db, path, 1));
    int16_t value;
    CHECK(egtb_get_uncached(db, index, EGTB_WHITE_TO_MOVE, &value) && value == 1);
    CHECK(!egtb_get_uncached(db, eg_position_count(&indexer), EGTB_WHITE_TO_MOVE, &value));
    CHECK(egtb_close(db)); db = NULL;
    CHECK(run(dir, "W:WK49:B43", true, "1. 49x38 {1, 49x32 or 49x27 or 49x21 or 49x16} 2-0\n"));
    CHECK(run(dir, "W:WK47:B41", true, "1. 47x36 {1} 2-0\n"));
    CHECK(run(dir, "B:W8:BK2", true, "1... 2x13 {1, 2x19 or 2x24 or 2x30 or 2x35} 0-2\n"));
    CHECK(run(dir, "W:WK50:B45", true, "1-1\n"));
    CHECK(run(dir, "W:W:B1", true, "0-2\n"));
    CHECK(run(dir, "W:WK49:B49", false, "invalid FEN"));
    CHECK(run(dir, "W:W1:B40", false, "invalid FEN"));
    CHECK(run(dir, "W:", false, "invalid FEN"));
    CHECK(run(dir, "W:WK49:B43,", false, "invalid FEN"));
    CHECK(run(dir, "W:WK49:BK43", false, "1wX-0wO-1bX-0bO.dtm"));
    CHECK(egtb_open_readwrite(&db, path, 1));
    CHECK(egtb_set(db, index, EGTB_WHITE_TO_MOVE, 3));
    CHECK(egtb_close(db)); db = NULL;
    CHECK(run(dir, "W:WK49:B43", false, "inconsistent DTM"));
    result = 0;
done:
    if (db) egtb_close(db);
    eg_indexer_destroy(&indexer);
    compat_unlink(path); remove_directory(dir);
    return result;
}
