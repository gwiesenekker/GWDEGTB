#define _POSIX_C_SOURCE 200809L
#include "egtb.h"
#include "wdl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zstd.h>

static void put(unsigned char *p, uint64_t value, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i)
        p[i] = (unsigned char)(value >> (8 * i));
}

static uint64_t get(const unsigned char *p, unsigned bytes)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < bytes; ++i)
        value |= (uint64_t)p[i] << (8 * i);
    return value;
}

static uint32_t crc(const unsigned char *p, size_t count)
{
    uint32_t c = UINT32_MAX;
    while (count-- != 0) {
        c ^= *p++;
        for (unsigned bit = 0; bit < 8; ++bit)
            c = (c >> 1) ^ ((c & 1) ? UINT32_C(0x82f63b78) : 0);
    }
    return ~c;
}

/* Independent fixtures: v2 paired bytes, v3 planar bytes, or a deliberately
 * supplied v4 encoded page. Header/directory encoding does not use egtb.c. */
static bool fixture(const char *path, unsigned version,
                    const unsigned char *stream, size_t size, uint32_t checksum)
{
    unsigned char header[64] = {'I','P','D','E','G','T','B',0};
    unsigned char directory[20] = {0}, block[4], compressed[4096];
    size_t length = ZSTD_compress(compressed, sizeof(compressed), stream, size, 1);
    FILE *file;
    bool ok;
    if (ZSTD_isError(length))
        return false;
    header[8] = (unsigned char)version;
    header[9] = 1; /* exact layout */
    put(header + 10, 64, 2);
    put(header + 12, 1024, 4);
    put(header + 16, 511, 8);
    put(header + 24, version == 2 ? 1 : 2, 8);
    put(header + 32, 64, 8);
    put(header + 40, version == 2 ? 74 : 84, 8);
    put(header + 50, 1, 2);
    put(directory, version == 2 ? 74 : 84, 8);
    put(directory + 8, length, 2);
    put(block, checksum, 4);
    file = fopen(path, "wb");
    if (file == NULL)
        return false;
    ok = fwrite(header, 1, 64, file) == 64 &&
         fwrite(directory, 1, version == 2 ? 10 : 20, file) ==
             (size_t)(version == 2 ? 10 : 20) &&
         fwrite(block, 1, 4, file) == 4 &&
         fwrite(compressed, 1, length, file) == length;
    return fclose(file) == 0 && ok;
}

static bool compare(const char *a, const char *b)
{
    Egtb *left = NULL, *right = NULL;
    bool ok = false;
    if (!egtb_open_readonly(&left, a, 32) ||
        !egtb_open_readonly(&right, b, 32) ||
        egtb_maximum_index(left) != egtb_maximum_index(right))
        goto done;
    for (uint64_t i = 0; i <= egtb_maximum_index(left); ++i)
        for (unsigned side = 0; side < 2; ++side) {
            int16_t x, y;
            if (!egtb_get(left, i, (EgtbSide)side, &x) ||
                !egtb_get(right, i, (EgtbSide)side, &y) || x != y) {
                fprintf(stderr, "mismatch at index %llu side %u\n",
                        (unsigned long long)i, side);
                goto done;
            }
        }
    ok = true;
done:
    if (left != NULL && !egtb_close(left)) ok = false;
    if (right != NULL && !egtb_close(right)) ok = false;
    return ok;
}

static bool legacy(const char *path, unsigned version)
{
    unsigned char bytes[1024];
    Egtb *db = NULL;
    EgtbView *view = NULL;
    EgtbResident *resident = NULL;
    bool ok = false;
    memset(bytes, 0x80, sizeof(bytes));
    for (size_t i = 0; i < 512; ++i)
        bytes[version == 2 ? i * 2 : i] = (unsigned char)(i % 256);
    if (!fixture(path, version, bytes, sizeof(bytes), crc(bytes, sizeof(bytes))))
        return false;
    for (unsigned pass = 0; pass < 2; ++pass) {
        EgtbSequentialReader reader;
        if (!egtb_open_readonly(&db, path, 2) ||
            !egtb_resident_load(&resident, db, 2) ||
            !egtb_view_create(&view, db, 2, false) ||
            !egtb_sequential_reader_init(&reader, view, 0, 512))
            goto done;
        for (uint64_t i = 0; i < 512; ++i) {
            unsigned byte = (unsigned)(i % 256);
            int16_t expected = byte == 128 ? EGTB_DRAW :
                egtb_decode_dtm((int16_t)(byte < 128 ? (int)byte : (int)byte - 256));
            int16_t w, b, rw, rb, cw;
            if (!egtb_sequential_reader_next(&reader, &w, &b) ||
                !egtb_resident_get_pair(resident, i, &rw, &rb) ||
                !egtb_get(db, i, EGTB_WHITE_TO_MOVE, &cw) ||
                w != expected || rw != expected || cw != expected ||
                b != EGTB_DRAW || rb != EGTB_DRAW)
                goto done;
        }
        egtb_resident_destroy(resident); resident = NULL;
        if (!egtb_view_close(view)) { view = NULL; goto done; }
        view = NULL;
        if (!egtb_close(db)) { db = NULL; goto done; }
        db = NULL;
        if (!egtb_compact(path, 1, 2)) goto done;
    }
    /* Legacy writes must reject wide values without dirtying the cache. */
    if (!egtb_open_readwrite(&db, path, 2) ||
        egtb_set(db, 0, EGTB_WHITE_TO_MOVE, 255) ||
        !egtb_set_pair(db, 0, 253, -254))
        goto done;
    ok = true;
done:
    egtb_resident_destroy(resident);
    if (view != NULL && !egtb_view_close(view)) ok = false;
    if (db != NULL && !egtb_close(db)) ok = false;
    return ok;
}

static int16_t code_at(uint64_t i)
{
    return i == 0 ? EGTB_STORED_DRAW : (int16_t)((int)i - 16384);
}

static bool wide(const char *path, const char *wdl_path)
{
    Egtb *db = NULL;
    EgtbView *view = NULL;
    EgtbResident *resident = NULL;
    Wdl *wdl = NULL;
    EgtbEntry page[512];
    EgtbCreateOptions options = {2, 20, 1};
    uint64_t *histogram = calloc(2 * 65536, sizeof(*histogram));
    bool ok = false;
    unlink(path);
    if (histogram == NULL || !egtb_create(&db, path, 32767, 1024, &options) ||
        egtb_positions_per_page(db) != 512 ||
        !egtb_view_create(&view, db, 2, true))
        goto done;
    for (uint64_t p = 0; p < 64; ++p) {
        for (unsigned i = 0; i < 512; ++i) {
            uint64_t index = p * 512 + i;
            page[i] = (EgtbEntry){code_at(index), code_at(32767 - index)};
        }
        if (p % 2 == 0) {
            if (!egtb_view_write_page(view, p, page, 512)) goto done;
        } else {
            /* Exercise dirty evictions and incremental checksums with high
             * code bits as well as the completed-page compilation path. */
            for (unsigned i = 0; i < 512; ++i)
                if (!egtb_view_set(view, p * 512 + i, EGTB_WHITE_TO_MOVE,
                                    egtb_decode_dtm(page[i].white_to_move)) ||
                    !egtb_view_set(view, p * 512 + i, EGTB_BLACK_TO_MOVE,
                                    egtb_decode_dtm(page[i].black_to_move)))
                    goto done;
        }
    }
    if (!egtb_view_close(view)) { view = NULL; goto done; }
    view = NULL;
    if (!egtb_close(db)) { db = NULL; goto done; }
    db = NULL;
    for (unsigned pass = 0; pass < 2; ++pass) {
        EgtbSequentialReader reader;
        if (!egtb_open_readonly(&db, path, 2) ||
            !egtb_resident_load(&resident, db, 3) ||
            egtb_resident_bytes(resident) != 32768 * sizeof(EgtbEntry) ||
            !egtb_resident_dtm_histogram(resident, histogram, 65536) ||
            !egtb_view_create(&view, db, 2, false) ||
            !egtb_sequential_reader_init(&reader, view, 0, 32768)) goto done;
        for (uint64_t i = 0; i < 32768; ++i) {
            int16_t w, b, rw, rb, cw, cb;
            int16_t ew = egtb_decode_dtm(code_at(i));
            int16_t eb = egtb_decode_dtm(code_at(32767 - i));
            if (!egtb_sequential_reader_next(&reader, &w, &b) ||
                !egtb_resident_get_pair(resident, i, &rw, &rb) ||
                !egtb_get(db, i, EGTB_WHITE_TO_MOVE, &cw) ||
                !egtb_get(db, i, EGTB_BLACK_TO_MOVE, &cb) ||
                w != ew || b != eb || rw != ew || rb != eb || cw != ew || cb != eb ||
                histogram[(uint16_t)ew] != 1 || histogram[65536 + (uint16_t)eb] != 1)
                goto done;
        }
        egtb_resident_destroy(resident); resident = NULL;
        if (!egtb_view_close(view)) { view = NULL; goto done; }
        view = NULL;
        if (!egtb_close(db)) { db = NULL; goto done; }
        db = NULL;
        if (!egtb_compact(path, 3, 2)) goto done;
    }
    if (!wdl_compile(path, wdl_path, 1, 2, NULL, NULL) ||
        !wdl_open(&wdl, wdl_path, 2, 1, 2)) goto done;
    for (uint64_t i = 0; i < 32768; ++i)
        for (unsigned side = 0; side < 2; ++side) {
            int16_t dtm = egtb_decode_dtm(code_at(side == 0 ? i : 32767 - i));
            WdlResult result, expected = dtm == EGTB_DRAW ? WDL_DRAW :
                (dtm > 0 ? WDL_WIN : WDL_LOSS);
            if (!wdl_get(wdl, i, (EgtbSide)side, &result) || result != expected)
                goto done;
        }
    ok = true;
done:
    free(histogram);
    egtb_resident_destroy(resident);
    if (view != NULL && !egtb_view_close(view)) ok = false;
    if (db != NULL && !egtb_close(db)) ok = false;
    if (wdl != NULL && !wdl_close(wdl)) ok = false;
    return ok;
}

static bool malformed(const char *path)
{
    unsigned char bytes[1536] = {0};
    /* truncated compact page, truncated escape, trailing bytes,
     * invalid extended code, noncanonical escape, and CRC corruption. */
    for (unsigned mode = 0; mode < 6; ++mode) {
        Egtb *db = NULL;
        int16_t value;
        size_t size = 512;
        memset(bytes, 0, sizeof(bytes));
        if (mode == 0) size = 511;
        if (mode == 1) bytes[511] = 0x7f;
        if (mode == 2) size = 513;
        if (mode == 3) { bytes[0] = 0x7f; bytes[1] = 0xff; bytes[2] = 0x7f; size = 514; }
        if (mode == 4) { bytes[0] = 0x7f; size = 514; }
        if (!fixture(path, 4, bytes, size, 0) ||
            !egtb_open_readonly(&db, path, 2)) return false;
        static const char *const errors[] = {
            "truncated", "truncated", "trailing", "invalid DTM",
            "noncanonical", "CRC32C"
        };
        bool accepted = egtb_get(db, 0, EGTB_WHITE_TO_MOVE, &value);
        bool expected_error = strstr(egtb_last_error(), errors[mode]) != NULL;
        if (!egtb_close(db) || accepted || !expected_error) return false;
    }
    return true;
}

static bool stream_sizes(const char *path)
{
    for (unsigned extended = 0; extended < 2; ++extended) {
        Egtb *db = NULL;
        EgtbView *view = NULL;
        EgtbCreateOptions options = {2, 0, 1};
        EgtbEntry values[512];
        unsigned char directory[10], packed[4096], stream[1536];
        FILE *file;
        uint64_t offset;
        size_t length, decoded;
        bool ok;
        unlink(path);
        if (!egtb_create(&db, path, 511, 1024, &options) ||
            !egtb_view_create(&view, db, 2, true)) {
            if (db != NULL) egtb_close(db);
            return false;
        }
        for (unsigned i = 0; i < 512; ++i)
            values[i] = (EgtbEntry){extended ? 300 : 1, EGTB_STORED_DRAW};
        ok = egtb_view_write_page(view, 0, values, 512);
        ok = egtb_view_close(view) && ok;
        ok = egtb_close(db) && ok;
        if (!ok) return false;
        file = fopen(path, "rb");
        if (file == NULL) return false;
        ok = fseek(file, 64, SEEK_SET) == 0 &&
             fread(directory, 1, 10, file) == 10;
        offset = ok ? get(directory, 8) : 0;
        length = ok ? (size_t)get(directory + 8, 2) : 0;
        ok = ok && length <= sizeof(packed) && offset != 0 &&
             fseek(file, (long)(offset + 4), SEEK_SET) == 0 &&
             fread(packed, 1, length, file) == length;
        ok = fclose(file) == 0 && ok;
        if (!ok) return false;
        decoded = ZSTD_decompress(stream, sizeof(stream), packed, length);
        if (decoded != (extended ? 1536u : 512u)) return false;
    }
    return true;
}

int main(int argc, char **argv)
{
    char directory[] = "/tmp/ipd-dtm16-XXXXXX";
    char path[256], wdl_path[256];
    bool ok;
    if (argc == 4 && strcmp(argv[1], "--compare") == 0) {
        ok = compare(argv[2], argv[3]);
    } else {
        if (mkdtemp(directory) == NULL) return EXIT_FAILURE;
        snprintf(path, sizeof(path), "%s/test.dtm", directory);
        snprintf(wdl_path, sizeof(wdl_path), "%s/test.wdl", directory);
        ok = legacy(path, 2) && legacy(path, 3) && wide(path, wdl_path) &&
             stream_sizes(path) && malformed(path);
        unlink(path); unlink(wdl_path); rmdir(directory);
    }
    if (!ok) fprintf(stderr, "DTM16 test failed: %s / %s\n", egtb_last_error(), wdl_last_error());
    else puts("DTM16 validation: PASS");
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
