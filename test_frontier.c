/* White-box test: also prove disjoint blocks require no file I/O. */
#include "frontier.c"
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)
typedef struct { uint64_t first, end, count, sum; } Result;
static bool collect(uint64_t index, void *opaque)
{
    Result *r = opaque;
    REQUIRE(index >= r->first && index < r->end);
    ++r->count; r->sum += index;
    return true;
}
static bool cancel(uint64_t index, void *opaque)
{
    (void)index; (void)opaque;
    return false;
}
int main(void)
{
    FrontierStore *store;
    REQUIRE(frontier_store_create(&store, 2, 1));
    /* Reverse order, multiple blocks, duplicate, and partial tail. */
    for (unsigned i = 0; i < 1100; ++i)
        REQUIRE(frontier_store_append(store, 0, EGTB_WHITE_TO_MOVE, 3, 1099-i));
    REQUIRE(frontier_store_append(store, 0, EGTB_WHITE_TO_MOVE, 3, 500));
    for (uint64_t first = 0; first < 1200; first += 37) {
        Result r = {first, first + 37, 0, 0};
        uint64_t count = 0, sum = 0;
        for (uint64_t i = first; i < r.end && i < 1100; ++i) { ++count; sum += i; }
        if (first <= 500 && r.end > 500) { ++count; sum += 500; }
        REQUIRE(frontier_store_visit_range(store, 0, EGTB_WHITE_TO_MOVE,
                                            3, first, r.end, collect, &r));
        REQUIRE(r.count == count && r.sum == sum);
    }
    Result r = {0, 1100, 0, 0};
    REQUIRE(frontier_store_visit(store, 0, EGTB_WHITE_TO_MOVE, 3, collect, &r));
    REQUIRE(r.count == 1101 && r.sum == 1100 * 1099 / 2 + 500);
    REQUIRE(!frontier_store_visit_range(store, 0, EGTB_WHITE_TO_MOVE,
                                         3, 0, 1100, cancel, NULL));
    REQUIRE(frontier_store_append(store, 1, EGTB_BLACK_TO_MOVE, 0, UINT64_MAX));
    REQUIRE(frontier_store_finish(store));
    FrontierStream *s = get_stream(store, 1, EGTB_BLACK_TO_MOVE, 0);
    REQUIRE(s->blocks[0].minimum_index == UINT64_MAX);
    REQUIRE(s->blocks[0].maximum_index == UINT64_MAX);
    REQUIRE(!frontier_store_visit(store, 1, EGTB_BLACK_TO_MOVE, 0, cancel, NULL));
    REQUIRE(close(store->owners[0].descriptor) == 0);
    store->owners[0].descriptor = -1;
    REQUIRE(frontier_store_visit_range(store, 0, EGTB_WHITE_TO_MOVE,
                                        3, 1100, 2000, cancel, NULL));
    REQUIRE(frontier_store_visit_range(store, 0, EGTB_WHITE_TO_MOVE,
                                        3, 500, 500, cancel, NULL));
    REQUIRE(!frontier_store_visit_range(store, 0, EGTB_WHITE_TO_MOVE,
                                         3, 500, 501, cancel, NULL));
    frontier_store_destroy(store);
    puts("frontier range tests passed");
    return 0;
}
