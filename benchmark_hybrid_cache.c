/* Experimental segmented storage + previous-mapping recovery. Not linked into
 * the library. The included driver uses the actual production page decoder. */
#define main segmented_benchmark_main
#include "benchmark_segmented_cache.c"
#undef main

typedef struct {
    Egtb *db;
    Segment **side[2];
    size_t count, pages, mask, bytes, per_side, previous_side;
    unsigned shift;
    CacheModulo modulo, previous_modulo;
} Hybrid;

/* The directory, capacities and previous mapping are changed only after every
 * worker has joined. Slot tags, sequences and payload remain atomic. Separate
 * side directories ensure growing WTM does not shift BTM's existing storage. */
static Hybrid *hybrid_create(Egtb *db, size_t bytes, size_t segment_bytes) {
    REQUIRE(db->planar && db->memory_page_size == 2048);
    REQUIRE(segment_bytes >= 2048 && !(segment_bytes & (segment_bytes - 1)));
    REQUIRE(bytes >= 2 * segment_bytes && bytes % (2 * segment_bytes) == 0);
    Hybrid *h = calloc(1, sizeof(*h)); REQUIRE(h);
    h->db = db; h->bytes = segment_bytes; h->pages = segment_bytes / 2048;
    h->mask = h->pages - 1;
    for (size_t n = h->pages; n > 1; n >>= 1) ++h->shift;
    h->count = bytes / (2 * segment_bytes); REQUIRE(h->count <= MAX_SEGMENTS);
    for (unsigned side = 0; side < 2; ++side) {
        h->side[side] = calloc(MAX_SEGMENTS, sizeof(Segment *)); REQUIRE(h->side[side]);
        for (size_t i = 0; i < h->count; ++i) {
            Segment *s = calloc(1, sizeof(*s)); REQUIRE(s);
            s->slots = compat_aligned_alloc(64, h->pages * sizeof(SharedSlot));
            s->words = malloc(segment_bytes); REQUIRE(s->slots && s->words);
            h->side[side][i] = s;
        }
    }
    h->per_side = h->count * h->pages; h->modulo = cache_modulo_init(h->per_side);
    return h;
}
static void hybrid_initialize(void *arg, unsigned id) {
    Hybrid *h = arg;
    for (size_t n = id; n < 2 * h->count; n += TEAM) {
        Segment *s = h->side[n / h->count][n % h->count];
        for (size_t i = 0; i < h->pages; ++i) {
            atomic_init(&s->slots[i].sequence, 0);
            atomic_init(&s->slots[i].tag, UINT64_MAX);
        }
        for (size_t i = 0; i < h->bytes / 8; ++i) atomic_init(&s->words[i], 0);
    }
}
static void hybrid_destroy(Hybrid *h) {
    if (!h) return;
    for (unsigned side = 0; side < 2; ++side) {
        for (size_t i = 0; i < h->count; ++i) {
            Segment *s = h->side[side][i]; free(s->words);
            compat_aligned_free(s->slots); free(s);
        }
        free(h->side[side]);
    }
    free(h);
}
static void hybrid_remember(Hybrid *h) {
    /* Discard the older mapping before installing another: never a chain. */
    h->previous_side = h->per_side; h->previous_modulo = h->modulo;
}
static void hybrid_update(Hybrid *h) {
    h->per_side = h->count * h->pages; h->modulo = cache_modulo_init(h->per_side);
}
static void hybrid_resize(Hybrid *h, size_t bytes) {
    REQUIRE(bytes >= 2*h->bytes && bytes % (2*h->bytes) == 0);
    size_t count = bytes/(2*h->bytes); REQUIRE(count<=MAX_SEGMENTS);
    if(count==h->count)return;
    hybrid_remember(h);
    if(count>h->count) {
        /* Construct only the new segments. Existing addresses never move. */
        Hybrid *extra=hybrid_create(h->db,2*(count-h->count)*h->bytes,h->bytes);
        team(hybrid_initialize,extra);
        for(unsigned side=0;side<2;++side) {
            for(size_t i=0;i<extra->count;++i)h->side[side][h->count+i]=extra->side[side][i];
            free(extra->side[side]);
        }
        free(extra);
    } else {
        for(unsigned side=0;side<2;++side)for(size_t i=count;i<h->count;++i) {
            Segment *s=h->side[side][i];free(s->words);compat_aligned_free(s->slots);free(s);
            h->side[side][i]=NULL;
        }
    }
    h->count=count;hybrid_update(h);
}
static void hybrid_transfer(Hybrid *from, Hybrid *to, size_t bytes) {
    REQUIRE(from->bytes == to->bytes && bytes % (2 * from->bytes) == 0);
    size_t n = bytes / (2 * from->bytes);
    REQUIRE(n && n < from->count && to->count + n <= MAX_SEGMENTS);
    Segment **moved = calloc(2 * n, sizeof(*moved)); REQUIRE(moved);
    hybrid_remember(from); hybrid_remember(to);
    for (unsigned side = 0; side < 2; ++side) for (size_t i = 0; i < n; ++i) {
        moved[side * n + i] = from->side[side][from->count - n + i];
        from->side[side][from->count - n + i] = NULL;
    }
    /* Quiescence permits reset of sequence numbers. All words already have
     * atomic lifetimes; the next publisher overwrites the entire page. */
    Clear clear = {moved, 2 * n, from->pages}; team(clear_range, &clear);
    for (unsigned side = 0; side < 2; ++side) for (size_t i = 0; i < n; ++i)
        to->side[side][to->count + i] = moved[side * n + i];
    from->count -= n; to->count += n;
    hybrid_update(from); hybrid_update(to); free(moved);
}

static _Thread_local uint64_t recovered, promoted, snapshot_retries;
static uint64_t rec[TEAM], pro[TEAM], retry[TEAM];
static bool EGTB_COLD_NOINLINE hybrid_miss(EgtbSharedProbe *p, uint64_t page,
    uint64_t physical, size_t logical, uint32_t entry, EgtbSide side, int16_t *value) {
    Hybrid *h = (Hybrid *)(void *)p->cache;
    Segment *dest = h->side[side][logical >> h->shift];
    size_t ds = logical & h->mask;
    if (h->previous_side) {
        size_t old = cache_modulo(page, h->previous_modulo);
        /* A shrink can remove the old address. Never dereference its segment.
         * Identical old/new slots have already failed the fast-path attempt. */
        if (old < h->per_side && old != logical) {
            Segment *source = h->side[side][old >> h->shift];
            size_t os = old & h->mask;
            SharedSlot *s = &source->slots[os];
            uint64_t seq = atomic_load_explicit(&s->sequence, memory_order_acquire);
            if (!(seq & 1) && atomic_load_explicit(&s->tag, memory_order_relaxed) == physical) {
                uint64_t snapshot[256];
                for (size_t i = 0; i < 256; ++i)
                    snapshot[i] = atomic_load_explicit(&source->words[os * 256 + i], memory_order_relaxed);
                atomic_thread_fence(memory_order_acquire);
                if (seq == atomic_load_explicit(&s->sequence, memory_order_relaxed)) {
                    int16_t stored; memcpy(&stored, (unsigned char *)snapshot + entry * 2, 2);
                    *value = egtb_decode_dtm(stored);
                    ++p->statistics.cache.hits; ++recovered;
                    /* Acquire only the destination AFTER validating a private
                     * snapshot. There is no source pin or two-lock cycle. */
                    SharedSlot *d = &dest->slots[ds];
                    seq = atomic_load_explicit(&d->sequence, memory_order_acquire);
                    if (!(seq & 1) && seq < UINT64_MAX - 1 &&
                        atomic_compare_exchange_strong_explicit(&d->sequence, &seq, seq + 1,
                            memory_order_acq_rel, memory_order_relaxed)) {
                        atomic_thread_fence(memory_order_release);
                        for (size_t i = 0; i < 256; ++i)
                            atomic_store_explicit(&dest->words[ds * 256 + i], snapshot[i], memory_order_relaxed);
                        atomic_store_explicit(&d->tag, physical, memory_order_relaxed);
                        atomic_store_explicit(&d->sequence, seq + 2, memory_order_release);
                        ++p->statistics.publications; ++promoted;
                    } else ++p->statistics.publication_conflicts;
                    return true;
                }
                ++snapshot_retries;
            }
        }
    }
    EgtbSharedCache adapter = {.backing=h->db, .slots=dest->slots,
        .words=dest->words, .words_per_page=256};
    EgtbSharedCache *saved = p->cache; p->cache = &adapter;
    bool ok = shared_probe_miss(p, physical, ds, entry, side, value);
    p->cache = saved; return ok;
}
static bool __attribute__((noinline)) hybrid_get(EgtbSharedProbe *p,
    uint64_t index, EgtbSide side, int16_t *value) {
    Hybrid *h = (Hybrid *)(void *)p->cache;
    uint64_t page; uint32_t entry; split_entry_index(h->db, index, &page, &entry);
    size_t logical = cache_modulo(page, h->modulo);
    Segment *segment = h->side[side][logical >> h->shift];
    size_t slot = logical & h->mask;
    uint64_t physical = page + (uint64_t)side * h->db->side_page_count;
    SharedSlot *s = &segment->slots[slot]; ++p->statistics.cache.lookups;
    uint64_t seq = atomic_load_explicit(&s->sequence, memory_order_acquire);
    if (seq & 1) ++p->statistics.busy_reads;
    else if (atomic_load_explicit(&s->tag, memory_order_relaxed) == physical) {
        uint64_t word = atomic_load_explicit(&segment->words[slot * 256 + entry / 4], memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        if (seq == atomic_load_explicit(&s->sequence, memory_order_relaxed)) {
            int16_t stored; memcpy(&stored, (unsigned char *)&word + (entry % 4) * 2, 2);
            *value = egtb_decode_dtm(stored); ++p->statistics.cache.hits; return true;
        }
        ++p->statistics.invalidated_reads;
    }
    return hybrid_miss(p, page, physical, logical, entry, side, value);
}
static void hybrid_probes_open(Run *r, Hybrid *h) {
    r->get = hybrid_get;
    for (unsigned i = 0; i < TEAM; ++i) {
        r->probes[i] = calloc(1, sizeof(*r->probes[i])); REQUIRE(r->probes[i]);
        REQUIRE(egtb_view_create(&r->probes[i]->scratch, r->db, 1, false));
        r->probes[i]->cache = (EgtbSharedCache *)(void *)h;
    }
}
static void hybrid_probes_close(Run *r) {
    for (unsigned i = 0; i < TEAM; ++i) {
        r->probes[i]->cache = NULL; egtb_shared_probe_destroy(r->probes[i]);
    }
}
static void hybrid_worker(void *arg, unsigned id) {
    recovered = promoted = snapshot_retries = 0; probe_range(arg, id);
    rec[id] = recovered; pro[id] = promoted; retry[id] = snapshot_retries;
}
static void hybrid_measure(Run *r, const char *label, unsigned round, const char *stage) {
    double start = shared_clock(); team(hybrid_worker, r); double seconds = shared_clock() - start;
    uint64_t sum=0, decodes=0, recovery=0, promotion=0, retries=0;
    for (unsigned i=0;i<TEAM;++i) {
        sum+=r->sums[i]; decodes+=r->decodes[i]; recovery+=rec[i]; promotion+=pro[i]; retries+=retry[i];
    }
    printf("HYBRID,%s,%u,%s,%.6f,decodes=%"PRIu64",recovered=%"PRIu64",promoted=%"PRIu64",snapshot-retries=%"PRIu64",checksum=%"PRIu64"\n",
        label,round,stage,seconds,decodes,recovery,promotion,retries,sum);
}
static void boundary_check(Run *r) {
    for (unsigned end=0;end<2;++end) for(unsigned side=0;side<2;++side) {
        int16_t value,expected; uint64_t index=end?r->db->maximum_index:0;
        REQUIRE(r->get(r->probes[0], index, (EgtbSide)side, &value));
        REQUIRE(egtb_resident_get(r->truth,index,(EgtbSide)side,&expected)); REQUIRE(value==expected);
    }
}
static void hybrid_resize_check(const char *path) {
    Egtb *db;EgtbResident *truth;REQUIRE(egtb_open_readonly(&db,path,1));
    REQUIRE(egtb_resident_load(&truth,db,TEAM));
    Hybrid *h=hybrid_create(db,8<<20,1<<20);team(hybrid_initialize,h);
    Segment *first[2]={h->side[0][0],h->side[1][0]};
    Run r={.db=db,.truth=truth,.count=50000,.span=db->side_page_count,
        .seed=713,.workload=1,.validate=true};
    hybrid_probes_open(&r,h);boundary_check(&r);hybrid_measure(&r,"hybrid",0,"initial");
    size_t sizes[]={12,4,14,2,8,8};
    for(unsigned i=0;i<sizeof(sizes)/sizeof(sizes[0]);++i) {
        hybrid_resize(h,sizes[i]<<20);
        REQUIRE(h->side[0][0]==first[0] && h->side[1][0]==first[1]);
        boundary_check(&r);hybrid_measure(&r,"hybrid",i,"resized");
    }
    h->previous_side=0;boundary_check(&r);hybrid_measure(&r,"hybrid",0,"retired");
    hybrid_probes_close(&r);hybrid_destroy(h);egtb_resident_destroy(truth);REQUIRE(egtb_close(db));
    puts("PASS incremental allocation/free, surviving addresses unchanged");
}
static void hybrid_benchmark(const char *path, const char *path2, size_t mib,
    size_t segmib, unsigned rounds, uint64_t queries, int workload) {
    Egtb *db[2]; EgtbResident *truth[2];
    REQUIRE(egtb_open_readonly(&db[0],path,1)); REQUIRE(egtb_open_readonly(&db[1],path2,1));
    for (unsigned i=0;i<2;++i) REQUIRE(egtb_resident_load(&truth[i],db[i],TEAM));
    for(unsigned round=0;round<rounds;++round) for(unsigned order=0;order<4;++order) {
        unsigned mode=round%2?3-order:order;
        const char *label=mode==0?"discard":mode==1?"migrate":mode==2?"split-segments":"hybrid";
        Run r[2]; Hybrid *h[2]={NULL,NULL}; size_t bytes=mib<<20, delta=bytes/4;
        for(unsigned i=0;i<2;++i) {
            r[i]=(Run){.db=db[i],.truth=truth[i],.count=queries,.span=bytes/4096,
                .seed=i?91:51,.workload=workload,.validate=true,.get=flat_get};
            if(r[i].span>db[i]->side_page_count)r[i].span=db[i]->side_page_count;
            if(mode==3) {
                h[i]=hybrid_create(db[i],bytes/(i+1),segmib<<20);team(hybrid_initialize,h[i]);
                hybrid_probes_open(&r[i],h[i]);
            } else {
                if(mode==2){r[i].seg=seg_create(db[i],bytes/(i+1),segmib<<20);r[i].get=seg_get;}
                else REQUIRE(egtb_shared_cache_create_threads(&r[i].flat,db[i],bytes/(i+1),TEAM));
                probes_open(&r[i]);
            }
            boundary_check(&r[i]);hybrid_measure(&r[i],label,round,i?"receiver-warmup":"donor-warmup");
        }
        for(unsigned cycle=0;cycle<3;++cycle) {
            bool back=cycle==1; double t=shared_clock();
            if(mode==3) hybrid_transfer(h[back?1:0],h[back?0:1],delta);
            else if(mode==2) seg_transfer(r[back?1:0].seg,r[back?0:1].seg,delta);
            else for(unsigned n=0;n<2;++n) {
                unsigned i=back?1-n:n;size_t target=i?(back?bytes/2:bytes/2+delta):(back?bytes:bytes-delta);
                REQUIRE(mode==0?egtb_shared_cache_discard_resize(r[i].flat,target):egtb_shared_cache_resize(r[i].flat,target));
            }
            printf("TRANSFER,%s,%u,%u,%.6f\n",label,round,cycle,shared_clock()-t);
            for(unsigned i=0;i<2;++i) {
                boundary_check(&r[i]);hybrid_measure(&r[i],label,round,i?"receiver-after":"donor-after");
            }
        }
        for(unsigned i=0;i<2;++i) {
            if(h[i])h[i]->previous_side=0;
            hybrid_measure(&r[i],label,round,i?"receiver-retired":"donor-retired");
            if(h[i]){hybrid_probes_close(&r[i]);hybrid_destroy(h[i]);}
            else {probes_close(&r[i]);seg_destroy(r[i].seg);egtb_shared_cache_destroy(r[i].flat);}
        }
    }
    for(unsigned i=0;i<2;++i){egtb_resident_destroy(truth[i]);REQUIRE(egtb_close(db[i]));}
}
static void hybrid_trace(const char *path, size_t mib, size_t segmib, unsigned rounds) {
    Egtb *db;EgtbResident *truth;REQUIRE(egtb_open_readonly(&db,path,1));REQUIRE(egtb_resident_load(&truth,db,TEAM));
    for(unsigned round=0;round<rounds;++round)for(unsigned order=0;order<2;++order) {
        bool hy=order^(round&1);Hybrid *h=NULL;
        Run r={.db=db,.truth=truth,.get=flat_get,.validate=false};
        if(hy){h=hybrid_create(db,mib<<20,segmib<<20);team(hybrid_initialize,h);hybrid_probes_open(&r,h);}
        else{REQUIRE(egtb_shared_cache_create_threads(&r.flat,db,mib<<20,TEAM));probes_open(&r);}
        hybrid_measure(&r,hy?"hybrid":"flat",round,"cold");hybrid_measure(&r,hy?"hybrid":"flat",round,"warm");
        if(h){hybrid_probes_close(&r);hybrid_destroy(h);}else{probes_close(&r);egtb_shared_cache_destroy(r.flat);}
    }
    egtb_resident_destroy(truth);REQUIRE(egtb_close(db));
}
static void hybrid_large(unsigned rounds) {
    Egtb db={.readonly=true,.planar=true,.memory_page_size=2048,
        .side_page_count=UINT64_C(1000000000),.path="synthetic"};
    for(unsigned r=0;r<rounds;++r) {
        Hybrid *a=hybrid_create(&db,(size_t)32<<30,64<<20);
        Hybrid *b=hybrid_create(&db,(size_t)8<<30,64<<20);
        team(hybrid_initialize,a);team(hybrid_initialize,b);
        /* Payload initialized above; valid synthetic tags populate every slot.
         * Timing below excludes construction and initial tag population. */
        for(unsigned side=0;side<2;++side)for(size_t n=0;n<a->count;++n)
            for(size_t i=0;i<a->pages;++i)
                atomic_store_explicit(&a->side[side][n]->slots[i].tag,
                    n*a->pages+i+(uint64_t)side*db.side_page_count,memory_order_relaxed);
        double t=shared_clock();hybrid_transfer(a,b,(size_t)4<<30);
        printf("LARGE,hybrid,%u,forward,%.6f\n",r,shared_clock()-t);
        REQUIRE((a->per_side+b->per_side)*4096==((size_t)40<<30));
        t=shared_clock();hybrid_transfer(b,a,(size_t)4<<30);
        printf("LARGE,hybrid,%u,rollback,%.6f\n",r,shared_clock()-t);
        hybrid_destroy(a);hybrid_destroy(b);
    }
}
int main(int argc,char **argv) {
    setvbuf(stdout,NULL,_IOLBF,0);
    if(argc==3 && !strcmp(argv[1],"check")){hybrid_resize_check(argv[2]);return 0;}
    if(argc==3 && !strcmp(argv[1],"large")){hybrid_large((unsigned)atoi(argv[2]));return 0;}
    if(argc==9 && !strcmp(argv[1],"transfer")) {
        hybrid_benchmark(argv[2],argv[3],strtoull(argv[4],NULL,10),strtoull(argv[5],NULL,10),
            (unsigned)atoi(argv[6]),strtoull(argv[7],NULL,10),atoi(argv[8]));return 0;
    }
    if(argc==7 && !strcmp(argv[1],"trace")) {
        FILE *f=fopen(argv[3],"rb");REQUIRE(f);REQUIRE(fread(trace_counts,sizeof(uint64_t),TEAM,f)==TEAM);
        for(unsigned i=0;i<TEAM;++i){REQUIRE(trace_counts[i]<=1000000);trace_values[i]=malloc((trace_counts[i]+1)*8);REQUIRE(trace_values[i]);REQUIRE(fread(trace_values[i],8,trace_counts[i],f)==trace_counts[i]);}
        REQUIRE(!fclose(f));tracing=true;trace_repeats=100;
        hybrid_trace(argv[2],strtoull(argv[4],NULL,10),strtoull(argv[5],NULL,10),(unsigned)atoi(argv[6]));
        for(unsigned i=0;i<TEAM;++i)free(trace_values[i]);return 0;
    }
    fprintf(stderr,"transfer DB1 DB2 donorMiB segmentMiB rounds queries/thread workload\ntrace DB tracefile cacheMiB segmentMiB rounds\n");return 2;
}
