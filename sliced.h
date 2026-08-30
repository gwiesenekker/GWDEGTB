#ifndef SLICED_H
#define SLICED_H

#include "generator.h"
#include "material.h"

typedef struct {
    unsigned thread_count;
    uint32_t page_size;
    size_t writable_cache_pages;
    size_t slice_read_cache_pages;
    size_t verification_cache_pages;
    unsigned reserve_percent;
    int compression_level;
    EgtbExternalProbe external_probe;
    void *external_context;
    void *const *external_contexts;
    EgtbConsistencyReporter reporter;
    void *reporter_context;
    bool quiet;
} EgtbSlicedOptions;

const char *egtb_sliced_last_error(void);

/* Generate verified temporary slices and merge them into the normal full DTM. */
bool egtb_generate_sliced(Egtb **out, const char *path,
                          const EgtbMaterial *material,
                          const EgIndexer *full_indexer,
                          const EgtbSlicedOptions *options,
                          EgtbGenerationStatistics *statistics);

/* Remove a completed sliced workspace after final full-database verification. */
bool egtb_sliced_cleanup(const char *path, const EgtbMaterial *material);

#endif
