#ifndef FRONTIER_H
#define FRONTIER_H

#include "egtb.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct FrontierStore FrontierStore;

typedef bool (*FrontierVisitor)(uint64_t index, void *context);

bool frontier_store_create(FrontierStore **out, unsigned owner_count,
                           int compression_level);
void frontier_store_destroy(FrontierStore *store);

bool frontier_store_append(FrontierStore *store, unsigned owner,
                           EgtbSide side, int16_t dtm, uint64_t index);
bool frontier_store_finish(FrontierStore *store);
bool frontier_store_visit(FrontierStore *store, unsigned owner,
                          EgtbSide side, int16_t dtm,
                          FrontierVisitor visitor, void *context);
uint64_t frontier_store_count(const FrontierStore *store, EgtbSide side,
                              int16_t dtm);
uint16_t frontier_store_maximum_distance(const FrontierStore *store);
const char *frontier_last_error(void);

#endif
