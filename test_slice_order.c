#include "slice_order.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"failed line %d\n",__LINE__); return 1; } } while (0)
int main(void)
{
    const char *names[]={"row","column","tile2","tile3","diagonal"};
    SliceOrderEntry order[81];
    for (unsigned k=0;k<5;++k) for (unsigned rows=1;rows<=9;++rows)
        for (unsigned cols=1;cols<=9;++cols) {
            bool done[9][9]={{false}};
            unsigned n=slice_order_build(names[k],rows,cols,order);
            CHECK(n==rows*cols);
            for (unsigned i=0;i<n;++i) {
                unsigned r=order[i].white,c=order[i].black;
                CHECK(r<rows && c<cols && !done[r][c]);
                CHECK(!r || done[r-1][c]);
                CHECK(!c || done[r][c-1]);
                done[r][c]=true;
            }
        }
    CHECK(!slice_order_build("snake",9,9,order));
    CHECK(!slice_order_build("row",10,9,order));
    puts("slice orders: all 405 rectangular schedules have complete dependency-safe coverage");
    return 0;
}
