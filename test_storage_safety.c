#define _POSIX_C_SOURCE 200809L
#include "egtb.h"
#include "wdl.h"
#include "gwdegtb.h"
#include "endgame_index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s / %s / %s\n", \
    __LINE__, egtb_last_error(), wdl_last_error(), gwdegtb_last_error()); exit(1); } } while (0)

static uint64_t number(const unsigned char *p, unsigned n)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i) v |= (uint64_t)p[i] << (8*i);
    return v;
}

static void flip(const char *path, uint64_t offset)
{
    FILE *f = fopen(path, "r+b"); CHECK(f != NULL);
    CHECK(fseek(f, (long)offset, SEEK_SET) == 0);
    int c = fgetc(f); CHECK(c != EOF);
    CHECK(fseek(f, (long)offset, SEEK_SET) == 0);
    CHECK(fputc(c ^ 1, f) != EOF); CHECK(fclose(f) == 0);
}

static size_t block(const char *path, unsigned page, unsigned char *data,
                     uint64_t *offset)
{
    unsigned char entry[10]; FILE *f = fopen(path, "rb"); CHECK(f != NULL);
    CHECK(fseek(f, 64 + 10 * page, SEEK_SET) == 0);
    CHECK(fread(entry, 1, 10, f) == 10);
    *offset = number(entry, 8);
    size_t length = (size_t)number(entry + 8, 2) + 4;
    CHECK(length <= 4096 && *offset != 0);
    CHECK(fseek(f, (long)*offset, SEEK_SET) == 0);
    CHECK(fread(data, 1, length, f) == length); CHECK(fclose(f) == 0);
    return length;
}

int main(void)
{
    char dir[] = "/tmp/egtb-safety-XXXXXX", path[256], wpath[256];
    CHECK(mkdtemp(dir) != NULL);
    snprintf(path, sizeof(path), "%s/test.dtm", dir);
    snprintf(wpath, sizeof(wpath), "%s/test.wdl", dir);
    Egtb *d, *a, *b; EgtbView *av, *bv; int16_t v;
    EgtbCreateOptions options = {2, 80, 1};
    CHECK(egtb_create(&d, path, 2449, 1024, &options));
    CHECK(egtb_set(d, 0, 0, 1)); CHECK(egtb_set(d, 512, 0, 3));
    CHECK(egtb_set(d, 2048, 0, -2)); CHECK(egtb_close(d));
    unsigned char original[4096], copied[4096]; uint64_t offset, after;
    size_t length = block(path, 1, original, &offset);
    flip(path, offset);
    CHECK(egtb_open_readonly(&d, path, 2));
    CHECK(egtb_get(d, 0, 0, &v) && v == 1);
    CHECK(!egtb_get(d, 512, 0, &v));
    CHECK(egtb_get(d, 0, 0, &v) && v == 1);
    CHECK(egtb_view_create(&av, d, 2, false));
    CHECK(egtb_view_get(av, 0, 0, &v) && v == 1);
    CHECK(!egtb_view_get(av, 512, 0, &v));
    CHECK(egtb_view_get(av, 0, 0, &v) && v == 1);
    CHECK(egtb_view_close(av)); CHECK(egtb_close(d));
    CHECK(!egtb_compact_copy(path, 2));
    CHECK(block(path, 1, copied, &after) == length && after == offset);
    CHECK(copied[0] == (unsigned char)(original[0] ^ 1));
    flip(path, offset);
    CHECK(egtb_open_readonly(&a, path, 2)); CHECK(egtb_open_readonly(&b, path, 2));
    CHECK(egtb_view_create(&av, a, 2, false)); CHECK(egtb_view_create(&bv, b, 2, false));
    CHECK(egtb_view_close(av)); CHECK(egtb_close(a));
    CHECK(egtb_view_get(bv, 512, 0, &v) && v == 3);
    CHECK(!egtb_close(b)); /* Last reference must retain its live view. */
    CHECK(egtb_view_close(bv)); CHECK(egtb_close(b));
    CHECK(egtb_compact_copy(path, 2));
    CHECK(block(path, 1, copied, &after) == length);
    CHECK(memcmp(original, copied, length) == 0);
    CHECK(egtb_open_readonly(&d, path, 2));
    for (uint64_t i = 0; i < 2450; ++i)
        for (unsigned side = 0; side < 2; ++side) {
            int expected = side ? -1 : i == 0 ? 1 : i == 512 ? 3 : i == 2048 ? -2 : -1;
            CHECK(egtb_get(d, i, (EgtbSide)side, &v) && v == expected);
        }
    CHECK(egtb_close(d));

    CHECK(wdl_compile(path, wpath, 1, 2, NULL, NULL));
    flip(wpath, 64 + 14 + 10); /* Page one's directory checksum. */
    Wdl *w; WdlResult result;
    CHECK(wdl_open(&w, wpath, 1, 1, 2));
    CHECK(wdl_get(w, 0, 0, &result) && result == WDL_WIN);
    CHECK(!wdl_get(w, 2048, 0, &result));
    CHECK(wdl_get(w, 0, 0, &result) && result == WDL_WIN);
    CHECK(wdl_close(w));

    size_t bytes; CHECK(wdl_file_size(wpath, &bytes));
    void *image = malloc(bytes); CHECK(image != NULL);
    CHECK(wdl_file_load_into(wpath, image, bytes));
    CHECK(gwdegtb_wdl_compressed_attach("1wX-0wO-1bX-0bO", image, bytes));
    GwdegtbWdlProbe *probe; CHECK(gwdegtb_wdl_probe_create(2048, &probe));
    EgIndexer indexer; EgPosition p, q;
    CHECK(eg_indexer_init(&indexer, 0, 0, 1, 1));
    CHECK(eg_index_to_position(&indexer, 0, &p));
    CHECK(eg_index_to_position(&indexer, 2048, &q));
    CHECK(gwdegtb_wdl_lookup_probe_compact(probe,p.white_kings,0,p.black_kings,0,0)==1);
    CHECK(gwdegtb_wdl_lookup_probe_compact(probe,q.white_kings,0,q.black_kings,0,0)==INT16_MIN);
    CHECK(gwdegtb_wdl_lookup_probe_compact(probe,p.white_kings,0,p.black_kings,0,0)==1);
    eg_indexer_destroy(&indexer); gwdegtb_wdl_probe_destroy(probe);
    gwdegtb_wdl_compressed_unload_all(); free(image);
    unlink(path); unlink(wpath); rmdir(dir);
    puts("storage failure recovery, shared references and block-copy tests: PASS");
    return 0;
}
