#define _POSIX_C_SOURCE 200809L
#include "progress.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHECK(test) do { if (!(test)) { \
    fprintf(stderr, "progress test failed at line %d: %s\n", __LINE__, #test); \
    exit(EXIT_FAILURE); } } while (0)

static void *work(void *unused)
{
    uint64_t pending = 0;
    (void)unused;
    for (unsigned i = 0; i < 20005; ++i)
        egtb_progress_tick(&pending);
    egtb_progress_flush(&pending);
    return NULL;
}

int main(void)
{
    FILE *capture = tmpfile();
    CHECK(capture != NULL);
    int saved = dup(STDOUT_FILENO);
    CHECK(saved >= 0 && dup2(fileno(capture), STDOUT_FILENO) >= 0);
    egtb_progress_begin("disabled", 10, "positions");
    egtb_progress_add(1);
    egtb_progress_end(true);
    CHECK(egtb_progress_start(0));
    CHECK(egtb_progress_start(1));
    egtb_progress_begin("parallel", 80020, "positions");
    pthread_t workers[4];
    for (unsigned i = 0; i < 4; ++i)
        CHECK(pthread_create(&workers[i], NULL, work, NULL) == 0);
    for (unsigned i = 0; i < 4; ++i)
        CHECK(pthread_join(workers[i], NULL) == 0);
    egtb_progress_end(true);
    egtb_progress_begin("empty", 0, "positions");
    egtb_progress_end(true);
    egtb_progress_begin("unknown", UINT64_MAX, "");
    egtb_progress_end(true);
    egtb_progress_begin("timed", 1000, "positions");
    egtb_progress_add(500);
    /* The reporter must produce output even while workers are idle. */
    struct timespec delay = {1, 300000000};
    while (nanosleep(&delay, &delay) != 0) {}
    egtb_progress_end(false);
    egtb_progress_stop();
    CHECK(egtb_progress_start(1));
    egtb_progress_begin("restart", 1, "positions");
    egtb_progress_add(1);
    egtb_progress_end(true);
    egtb_progress_stop();
    fflush(stdout);
    CHECK(dup2(saved, STDOUT_FILENO) >= 0);
    close(saved);
    rewind(capture);
    char text[16384];
    size_t length = fread(text, 1, sizeof(text) - 1, capture);
    text[length] = '\0';
    fclose(capture);
    CHECK(strstr(text, "disabled") == NULL);
    CHECK(strstr(text, "80020/80020 positions (100.0%)") != NULL);
    CHECK(strstr(text, "0/0 positions (100.0%)") != NULL);
    CHECK(strstr(text, "ETA=unknown") != NULL);
    CHECK(strstr(text, "timed: running") != NULL);
    CHECK(strstr(text, "500/1000 positions (50.0%)") != NULL);
    CHECK(strstr(text, "timed: failed") != NULL);
    CHECK(strstr(text, "restart: completed") != NULL);
    CHECK(strstr(text, "nan") == NULL && strstr(text, "inf") == NULL);
    CHECK(text[0] == '[');
    puts("progress tests passed");
    return EXIT_SUCCESS;
}
