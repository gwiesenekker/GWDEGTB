#include "dtm_pv.h"
#include <string.h>
int main(int argc, char **argv)
{
    const char *dir = ".", *fen;
    if (argc == 2) fen = argv[1];
    else if (argc == 4 && !strcmp(argv[1], "-d")) {
        dir = argv[2]; fen = argv[3];
    } else {
        fprintf(stderr, "Usage: dtm_pv [-d directory] \"W:W...:B...\"\n");
        return 2;
    }
    char error[512];
    if (!gwdegtb_dtm_pv(dir, fen, stdout, error, sizeof error)) {
        fprintf(stderr, "PV failed: %s\n", error); return 1;
    }
    return 0;
}
