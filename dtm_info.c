#include "egtb.h"

#include <inttypes.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    EgtbFileInfo info;
    if (argc != 2) {
        fprintf(stderr, "Usage: dtm_info FILE.dtm\n");
        return 2;
    }
    if (!egtb_file_info(argv[1], &info)) {
        fprintf(stderr, "Cannot inspect %s: %s\n", argv[1], egtb_last_error());
        return 1;
    }
    printf("File: %s\n", argv[1]);
    printf("DTM format version: %u\n", info.format_version);
    printf("Recorded Zstd compression level: %d\n", info.compression_level);
    printf("Value width: %u bits\n", info.format_version == EGTB_FORMAT_VERSION ? 16u : 8u);
    printf("Layout: %s, %s\n", info.planar ? "WTM/BTM planar" : "paired WTM/BTM",
           info.exact_layout ? "exact" : "reserved slots");
    printf("Page size: %" PRIu32 " bytes\n", info.page_size);
    printf("Positions: %" PRIu64 " (maximum index %" PRIu64 ")\n",
           info.maximum_index + 1, info.maximum_index);
    printf("Stored pages: %" PRIu64 "\n", info.page_count);
    printf("Reserved page capacity: %u%%\n", info.reserve_percent);
    printf("Generator revision: not recorded in DTM files\n");
    return 0;
}
