#ifndef EGTB_SLICE_ORDER_H
#define EGTB_SLICE_ORDER_H

#include <stdbool.h>
#include <string.h>

typedef struct { unsigned white, black; } SliceOrderEntry;

/* Coordinates measure distance from promotion: both dependencies precede us
 * (white-1,black) and (white,black-1). Rows/columns are at most nine. */
static inline unsigned slice_order_build(const char *name, unsigned rows,
                                         unsigned columns, SliceOrderEntry out[81])
{
    unsigned n=0, tile=0;
    if (!name || !*name) name="row";
    if (!rows || rows>9 || !columns || columns>9) return 0;
    if (!strcmp(name,"tile2")) tile=2;
    else if (!strcmp(name,"tile3")) tile=3;
    if (tile) {
        for (unsigned r=0;r<rows;r+=tile)
            for (unsigned c=0;c<columns;c+=tile)
                for (unsigned i=r;i<rows && i<r+tile;++i)
                    for (unsigned j=c;j<columns && j<c+tile;++j)
                        out[n++]=(SliceOrderEntry){i,j};
    } else if (!strcmp(name,"row")) {
        for (unsigned i=0;i<rows;++i) for (unsigned j=0;j<columns;++j)
            out[n++]=(SliceOrderEntry){i,j};
    } else if (!strcmp(name,"column")) {
        for (unsigned j=0;j<columns;++j) for (unsigned i=0;i<rows;++i)
            out[n++]=(SliceOrderEntry){i,j};
    } else if (!strcmp(name,"diagonal")) {
        for (unsigned sum=0;sum<rows+columns-1;++sum)
            for (unsigned i=0;i<rows;++i)
                if (sum>=i && sum-i<columns) out[n++]=(SliceOrderEntry){i,sum-i};
    }
    return n;
}
#endif
