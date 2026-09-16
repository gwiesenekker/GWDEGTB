#define _POSIX_C_SOURCE 200809L
#include "wdl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "line %d: %s (WDL: %s; DTM: %s)\n", \
            __LINE__, #x, wdl_last_error(), egtb_last_error()); exit(1); \
} } while (0)

static int16_t value(uint64_t index, unsigned side)
{
    static const int16_t values[] = {-1, 0, 1, -2, 3, 255, -254, 511, -512};
    if ((index / 2048) % 3 == 0)
        return -1; /* implicit all-draw pages */
    return values[(index * 7 + side * 5) % 9];
}

static void test(const char *dir, uint64_t count, unsigned threads)
{
    char source[256], output[256];
    Egtb *dtm = NULL;
    Wdl *wdl = NULL;
    WdlStatistics stats, expected = {0};
    WdlStorageStatistics storage, actual;
    EgtbCreateOptions options = {8, 20, 1};
    snprintf(source, sizeof(source), "%s/output.dtm", dir);
    snprintf(output, sizeof(output), "%s/output.wdl", dir);
    CHECK(egtb_create(&dtm, source, count - 1, 2048, &options));
    for (uint64_t i = 0; i < count; ++i)
        CHECK(egtb_set_pair(dtm, i, value(i, 0), value(i, 1)));
    CHECK(egtb_close(dtm));
    CHECK(wdl_compile_threaded(source, output, 1, 8, threads, &stats, &storage));
    CHECK(wdl_open_threaded(&wdl, output, 2, 1, 2, threads));
    CHECK(wdl_maximum_index(wdl) == count - 1);
    size_t bytes = (size_t)((count + 1) / 2);
    unsigned char *bitmap = malloc(bytes);
    CHECK(bitmap);
    CHECK(wdl_decompress_into_threaded(wdl, bitmap, bytes, threads));
    for (uint64_t i = 0; i < count; ++i) {
        for (unsigned side = 0; side < 2; ++side) {
            int16_t v = value(i, side);
            WdlResult result, want = v == -1 ? WDL_DRAW : v > 0 ? WDL_WIN : WDL_LOSS;
            CHECK(wdl_get(wdl, i, (EgtbSide)side, &result));
            CHECK(result == want);
            CHECK(((bitmap[i / 2] >> ((i % 2) * 4 + side * 2)) & 3) == want);
            if (want == WDL_DRAW) ++expected.draws[side];
            else if (want == WDL_WIN) ++expected.wins[side];
            else ++expected.losses[side];
        }
    }
    if (count % 2) CHECK((bitmap[bytes - 1] & 0xf0) == 0);
    CHECK(memcmp(&stats, &expected, sizeof(stats)) == 0);
    CHECK(wdl_storage_statistics(wdl, &actual));
    CHECK(memcmp(&actual, &storage, sizeof(storage)) == 0);
    CHECK(wdl_close(wdl));
    free(bitmap);
    /* A failed replacement must leave the existing valid WDL untouched. */
    CHECK(!wdl_compile_threaded("/nonexistent/dtm", output, 1, 2, threads, NULL, NULL));
    CHECK(wdl_open_threaded(&wdl, output, 1, 1, 2, threads));
    CHECK(wdl_close(wdl));
    /* Corrupt a live block checksum after opening succeeded in earlier runs.
     * A worker read/decode/checksum failure must not replace the valid output. */
    if (count > 2048) {
        FILE *f = fopen(source, "r+b");
        unsigned char entry[10];
        uint64_t offset = 0;
        CHECK(f && fseek(f, 64, SEEK_SET) == 0);
        while (!offset) {
            CHECK(fread(entry, 1, sizeof(entry), f) == sizeof(entry));
            for (unsigned j = 0; j < 8; ++j)
                offset |= (uint64_t)entry[j] << (8 * j);
        }
        CHECK(fseeko(f, (off_t)offset, SEEK_SET) == 0);
        int byte = fgetc(f);
        CHECK(byte != EOF && fseeko(f, (off_t)offset, SEEK_SET) == 0);
        CHECK(fputc(byte ^ 0x80, f) != EOF && fclose(f) == 0);
        CHECK(!wdl_compile_threaded(source, output, 1, 2, threads, NULL, NULL));
        CHECK(wdl_open_threaded(&wdl, output, 1, 1, 2, threads));
        CHECK(wdl_close(wdl));
        f = fopen(source, "r+b");
        CHECK(f && fseeko(f, (off_t)offset, SEEK_SET) == 0);
        CHECK(fputc(byte, f) != EOF && fclose(f) == 0);
    }
    CHECK(unlink(output) == 0);
    CHECK(wdl_open_threaded(&wdl, output, 1, 1, 2, threads));
    CHECK(wdl_close(wdl));
    CHECK(unlink(output) == 0);
    CHECK(truncate(source, 64) == 0);
    CHECK(!wdl_compile_threaded(source, output, 1, 2, threads, NULL, NULL));
    CHECK(access(output, F_OK) != 0);
    CHECK(unlink(source) == 0);
}

static void test_loading(const char *dir)
{
    char path[256];snprintf(path,sizeof path,"%s/raw.wdl",dir);
    size_t size=8u*1024u*1024u+137;
    unsigned char *reference=malloc(size), *loaded=malloc(size+2);
    CHECK(reference && loaded);
    for(size_t i=0;i<size;i++) reference[i]=(unsigned char)((i*37)^(i>>11));
    FILE *f=fopen(path,"wb");CHECK(f);
    CHECK(fwrite(reference,1,size,f)==size);CHECK(fclose(f)==0);
    unsigned counts[]={1,2,4,16,256};
    for(unsigned i=0;i<sizeof counts/sizeof counts[0];i++){
        memset(loaded,0,size+2);loaded[0]=123;loaded[size+1]=231;
        CHECK(wdl_file_load_into_threaded(path,loaded+1,size,counts[i]));
        CHECK(!memcmp(reference,loaded+1,size));
        CHECK(loaded[0]==123 && loaded[size+1]==231);
    }
    CHECK(!wdl_file_load_into_threaded(path,loaded,size-1,4));
    CHECK(!wdl_file_load_into_threaded(path,loaded,size+1,4));
    CHECK(!wdl_file_load_into_threaded(path,loaded,size,0));
    CHECK(!wdl_file_load_into_threaded(path,loaded,size,257));
    CHECK(!wdl_file_load_into_threaded(path,NULL,size,4));
    CHECK(unlink(path)==0);
    CHECK(!wdl_file_load_into_threaded(path,loaded,size,4));
    free(reference);free(loaded);
}

int main(void)
{
    char dir[] = "/tmp/gwdegtb-wdl-compile-XXXXXX";
    CHECK(mkdtemp(dir));
    test_loading(dir);
    CHECK(!wdl_compile_threaded("x", "y", 1, 2, 0, NULL, NULL));
    CHECK(!wdl_compile_threaded("x", "y", 1, 2, 257, NULL, NULL));
    CHECK(setenv("EGTB_WDL_THREADS", "0", 1) == 0);
    CHECK(wdl_default_threads() == 0);
    CHECK(setenv("EGTB_WDL_THREADS", "8", 1) == 0);
    CHECK(wdl_default_threads() == 8);
    CHECK(unsetenv("EGTB_WDL_THREADS") == 0);
    CHECK(wdl_default_threads() == 4);
    test(dir, 1, 256);
    test(dir, 2047, 8);
    test(dir, 2048, 2);
    test(dir, 2049, 16);
    test(dir, 2048 * 1031 + 17, 1); /* multiple compressed batches */
    test(dir, 2048 * 4099 + 17, 4); /* multiple batches per worker */
    CHECK(rmdir(dir) == 0);
    puts("Parallel WDL compilation: PASS");
    return 0;
}
