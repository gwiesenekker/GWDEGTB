/* Isolated two-generation lazy migration experiment. Production is unchanged.
 * Reuse the benchmark's real decoder, reference checks and 16-thread driver. */
#define main segmented_benchmark_main
#include "benchmark_segmented_cache.c"
#undef main

/* Published/removed only while every worker is joined. One benchmark database
 * at a time; these globals are deliberately not a production API. */
static EgtbSharedCache *previous;
static _Atomic uint64_t *retired;
static _Thread_local uint64_t old_hits, promotions;
static uint64_t old_totals[TEAM], promotion_totals[TEAM];

static bool EGTB_COLD_NOINLINE lazy_miss(EgtbSharedProbe *p,
 uint64_t page,uint64_t physical,size_t slot,uint32_t entry,EgtbSide side,int16_t *value) {
 EgtbSharedCache *c=p->cache,*old=previous;
 if(old) {
  size_t os=(old->dense?(size_t)page:(size_t)cache_modulo(page,old->modulo))+(size_t)side*old->per_side;
  uint64_t bit=UINT64_C(1)<<(os%64);
  if(!(atomic_load_explicit(&retired[os/64],memory_order_relaxed)&bit) &&
     atomic_load_explicit(&old->slots[os].tag,memory_order_relaxed)==physical) {
   /* Old storage is immutable throughout this generation. New-cache eviction
    * can never change the value read here; no retry or old-slot pin is needed. */
   uint64_t word=atomic_load_explicit(&old->words[os*old->words_per_page+entry/4],memory_order_relaxed);
   int16_t stored;memcpy(&stored,(unsigned char*)&word+(entry%4)*2,2);
   *value=egtb_decode_dtm(stored);++p->statistics.cache.hits;++old_hits;
   SharedSlot *s=&c->slots[slot];
   uint64_t seq=atomic_load_explicit(&s->sequence,memory_order_acquire);
   if(!(seq&1) && seq<UINT64_MAX-1 && atomic_compare_exchange_strong_explicit(
       &s->sequence,&seq,seq+1,memory_order_acq_rel,memory_order_relaxed)) {
    atomic_thread_fence(memory_order_release);
    for(size_t i=0;i<c->words_per_page;i++) {
     word=atomic_load_explicit(&old->words[os*old->words_per_page+i],memory_order_relaxed);
     atomic_store_explicit(&c->words[slot*c->words_per_page+i],word,memory_order_relaxed);
    }
    atomic_store_explicit(&s->tag,physical,memory_order_relaxed);
    atomic_store_explicit(&s->sequence,seq+2,memory_order_release);
    /* Means 'retired old slot', never 'page permanently present in new cache'.
     * Later eviction merely causes a normal disk miss, not a wrong value. */
    atomic_fetch_or_explicit(&retired[os/64],bit,memory_order_relaxed);
    ++p->statistics.publications;++promotions;
   }else ++p->statistics.publication_conflicts;
   return true;
  }
 }
 return shared_probe_miss(p,physical,slot,entry,side,value);
}
static bool __attribute__((noinline)) lazy_get(EgtbSharedProbe *p,uint64_t index,EgtbSide side,int16_t *value) {
 EgtbSharedCache *c=p->cache;Egtb *db=c->backing;
 uint64_t page;uint32_t entry;split_entry_index(db,index,&page,&entry);
 size_t slot=(c->dense?(size_t)page:(size_t)cache_modulo(page,c->modulo))+(size_t)side*c->per_side;
 uint64_t physical=page+(uint64_t)side*db->side_page_count;
 ++p->statistics.cache.lookups;SharedSlot *s=&c->slots[slot];
 uint64_t seq=atomic_load_explicit(&s->sequence,memory_order_acquire);
 if(seq&1)++p->statistics.busy_reads;
 else if(atomic_load_explicit(&s->tag,memory_order_relaxed)==physical) {
  uint64_t word=atomic_load_explicit(&c->words[slot*c->words_per_page+entry/4],memory_order_relaxed);
  atomic_thread_fence(memory_order_acquire);
  if(seq==atomic_load_explicit(&s->sequence,memory_order_relaxed)) {
   int16_t stored;memcpy(&stored,(unsigned char*)&word+(entry%4)*2,2);
   *value=egtb_decode_dtm(stored);++p->statistics.cache.hits;return true;
  }++p->statistics.invalidated_reads;
 }
 return lazy_miss(p,page,physical,slot,entry,side,value);
}
static void lazy_range(void *arg,unsigned id) {
 old_hits=promotions=0;probe_range(arg,id);old_totals[id]=old_hits;promotion_totals[id]=promotions;
}
static double measure(Run *r,const char *label,unsigned round,const char *stage) {
 /* Same indirect dispatch for every candidate. */
 double start=shared_clock();team(lazy_range,r);double elapsed=shared_clock()-start;
 uint64_t sum=0,decodes=0,oh=0,pr=0;
 for(unsigned i=0;i<TEAM;i++){sum+=r->sums[i];decodes+=r->decodes[i];oh+=old_totals[i];pr+=promotion_totals[i];}
 printf("LAZY,%s,%u,%s,%.6f,decodes=%"PRIu64",old-hits=%"PRIu64",promotions=%"PRIu64",checksum=%"PRIu64"\n",label,round,stage,elapsed,decodes,oh,pr,sum);
 return elapsed;
}
static void drop_previous(void) {
 egtb_shared_cache_destroy(previous);previous=NULL;free(retired);retired=NULL;
}
int main(int argc,char **argv) {
 REQUIRE(argc==7);setvbuf(stdout,NULL,_IOLBF,0);
 size_t bytes=strtoull(argv[2],NULL,10)<<20, next=strtoull(argv[3],NULL,10)<<20;
 unsigned rounds=(unsigned)atoi(argv[4]);uint64_t queries=strtoull(argv[5],NULL,10);int workload=atoi(argv[6]);
 Egtb *db;EgtbResident *truth;REQUIRE(egtb_open_readonly(&db,argv[1],1));
 REQUIRE(db->planar && db->memory_page_size==2048);REQUIRE(egtb_resident_load(&truth,db,TEAM));
 for(unsigned round=0;round<rounds;round++)for(unsigned order=0;order<3;order++) {
  unsigned mode=round%2?2-order:order;const char *label=mode==0?"discard":mode==1?"migrate":"lazy";
  Run r={.db=db,.truth=truth,.count=queries,.span=bytes/2048,.seed=51,.workload=workload,.validate=true,.get=flat_get};
  if(r.span>db->side_page_count)r.span=db->side_page_count;
  REQUIRE(egtb_shared_cache_create_threads(&r.flat,db,bytes,TEAM));probes_open(&r);measure(&r,label,round,"warmup");
  /* Alternate capacity twice more, to exercise abandoning an unfinished old
   * generation, shrinking, repeated growth, and partial-page lookups. */
  for(unsigned cycle=0;cycle<3;cycle++) {
   size_t target=cycle==1?bytes:next;double t=shared_clock();
   if(mode==0)REQUIRE(egtb_shared_cache_discard_resize(r.flat,target));
   else if(mode==1)REQUIRE(egtb_shared_cache_resize(r.flat,target));
   else {
    probes_close(&r);drop_previous();previous=r.flat;
    REQUIRE(egtb_shared_cache_create_threads(&r.flat,db,target,TEAM));
    size_t words=(previous->capacity+63)/64;retired=malloc(words*sizeof(*retired));REQUIRE(retired);
    for(size_t i=0;i<words;i++)atomic_init(&retired[i],0);
    r.get=lazy_get;probes_open(&r);
   }
   printf("RESIZE,%s,%u,%u,%.6f\n",label,round,cycle,shared_clock()-t);
   measure(&r,label,round,"after");measure(&r,label,round,"warm");
  }
  drop_previous();measure(&r,label,round,"retired");probes_close(&r);egtb_shared_cache_destroy(r.flat);
 }
 egtb_resident_destroy(truth);REQUIRE(egtb_close(db));return 0;
}
