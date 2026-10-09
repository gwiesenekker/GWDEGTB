/* Experimental only. Include the storage implementation so the comparison uses
 * the real production decoder, miss path, checksums, and shared-cache API.
 * Never link this object with egtb.o. No production code is changed. */
#include "egtb.c"

#define BUCKETS 8
#define LANES (2 * BUCKETS)
#define TEAM 16
#define MAX_SEGMENTS 8192
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr,"FAIL %s:%d %s (%s)\n",__FILE__,__LINE__,#x,egtb_last_error()); abort(); } } while (0)
typedef struct { SharedSlot *slots; _Atomic uint64_t *words; } Segment;
typedef struct { Segment **segments; size_t count,base,mask; CacheModulo modulo; } Bucket;
typedef struct {
 Egtb *db; Bucket buckets[LANES]; size_t segment_bytes, slots, mask;
 unsigned shift; size_t next; bool linear;
} SegCache;

typedef void (*RangeFn)(void *,unsigned);
typedef struct { RangeFn fn; void *arg; unsigned id; } Task;
static void *task_run(void *p) { Task *t=p; t->fn(t->arg,t->id); return NULL; }
static void team(RangeFn fn,void *arg) {
 Task tasks[TEAM]; my_thread_t threads[TEAM-1]; bool started[TEAM-1];
 for(unsigned i=0;i<TEAM;i++) {
  tasks[i]=(Task){fn,arg,i};
  if(i<TEAM-1) { started[i]=!compat_thread_create(&threads[i],task_run,&tasks[i]); if(!started[i]) task_run(&tasks[i]); }
  else task_run(&tasks[i]);
 }
 for(unsigned i=0;i<TEAM-1;i++) if(started[i]) REQUIRE(!compat_thread_join(threads[i]));
}
static void bucket_update(SegCache *c, size_t b) {
 REQUIRE(c->buckets[b].count); c->buckets[b].modulo=cache_modulo_init(c->buckets[b].count*c->slots);
 c->buckets[b].base=1;while(c->buckets[b].base<=c->buckets[b].count/2)c->buckets[b].base*=2;
 c->buckets[b].mask=(c->buckets[b].count==c->buckets[b].base?c->buckets[b].base:2*c->buckets[b].base)-1;
}
static void seg_init_range(void *p,unsigned id) {
 SegCache *c=p;
 for(size_t b=id;b<LANES;b+=TEAM) {
  Bucket *q=&c->buckets[b];
  for(size_t n=0;n<q->count;n++) {
   Segment *s=q->segments[n];
   for(size_t i=0;i<c->slots;i++) { atomic_init(&s->slots[i].sequence,0); atomic_init(&s->slots[i].tag,UINT64_MAX); }
   for(size_t i=0;i<c->segment_bytes/8;i++) atomic_init(&s->words[i],0);
  }
 }
}
static SegCache *seg_create(Egtb *db,size_t bytes,size_t segment_bytes) {
 REQUIRE(db->planar && db->memory_page_size==2048);
 REQUIRE(bytes%segment_bytes==0 && bytes/segment_bytes>=LANES);
 SegCache *c=calloc(1,sizeof(*c)); REQUIRE(c);
 c->db=db;c->segment_bytes=segment_bytes;c->slots=segment_bytes/db->memory_page_size;
 c->linear=getenv("SEG_LINEAR") && atoi(getenv("SEG_LINEAR"));
 REQUIRE(!(c->slots&(c->slots-1)));c->mask=c->slots-1;
 for(size_t x=c->slots;x>1;x>>=1)++c->shift;
 for(size_t b=0;b<LANES;b++) {c->buckets[b].segments=calloc(MAX_SEGMENTS,sizeof(Segment*));REQUIRE(c->buckets[b].segments);}
 for(size_t n=0;n<bytes/segment_bytes;n++) {
  Bucket *b=&c->buckets[n%LANES]; REQUIRE(b->count<MAX_SEGMENTS);
  Segment *s=calloc(1,sizeof(*s));REQUIRE(s);
  s->slots=compat_aligned_alloc(64,c->slots*sizeof(SharedSlot));s->words=malloc(segment_bytes);
  REQUIRE(s->slots && s->words);b->segments[b->count++]=s;
 }
 for(size_t b=0;b<LANES;b++)bucket_update(c,b);
 team(seg_init_range,c);return c;
}
static void seg_destroy(SegCache *c) {
 if(!c)return;
 for(size_t b=0;b<LANES;b++) { Bucket *q=&c->buckets[b];for(size_t n=0;n<q->count;n++) {
  Segment *s=q->segments[n];free(s->words);compat_aligned_free(s->slots);free(s);
 }free(q->segments); }free(c);
}
typedef struct { Segment **s; size_t count,slots; } Clear;
static void clear_range(void *p,unsigned id) {
 Clear *c=p;
 for(size_t n=id;n<c->count;n+=TEAM)for(size_t i=0;i<c->slots;i++) {
  atomic_store_explicit(&c->s[n]->slots[i].tag,UINT64_MAX,memory_order_relaxed);
  atomic_store_explicit(&c->s[n]->slots[i].sequence,0,memory_order_relaxed);
 }
}
/* Quiescent transfer. Payload atomic objects retain their lifetime. Each miss
 * fully overwrites a page before publication. Only transferred tags need
 * invalidation; stale old-bucket tags cannot cause a false hit. */
static void seg_transfer(SegCache *from,SegCache *to,size_t bytes) {
 REQUIRE(from->segment_bytes==to->segment_bytes && bytes%from->segment_bytes==0);
 size_t count=bytes/from->segment_bytes; Segment **moved=calloc(count,sizeof(*moved));REQUIRE(moved);
 size_t available=0;for(size_t b=0;b<LANES;b++)available+=from->buckets[b].count-1;REQUIRE(count<=available);
 for(size_t n=0;n<count;n++) {
  size_t b=from->next%LANES;
  for(size_t j=1;j<LANES;j++){size_t k=(from->next+j)%LANES;if(from->buckets[k].count>from->buckets[b].count)b=k;}
  from->next=b+1;Bucket *q=&from->buckets[b]; REQUIRE(q->count>1);
  moved[n]=q->segments[--q->count];bucket_update(from,b);
 }
 Clear clear={moved,count,from->slots};team(clear_range,&clear);
 for(size_t n=0;n<count;n++) {
  size_t b=to->next%LANES;
  for(size_t j=1;j<LANES;j++){size_t k=(to->next+j)%LANES;if(to->buckets[k].count<to->buckets[b].count)b=k;}
  to->next=b+1;Bucket *q=&to->buckets[b];REQUIRE(q->count<MAX_SEGMENTS);
  q->segments[q->count++]=moved[n];bucket_update(to,b);
 }
 free(moved);
}
static bool EGTB_COLD_NOINLINE seg_miss(EgtbSharedProbe *p,Segment *segment,
 size_t slot,uint64_t physical,uint32_t entry,EgtbSide side,int16_t *value) {
 Egtb *db=((SegCache *)(void *)p->cache)->db;
 /* Same production miss path, including sampled timing. Keep the adapter
  * stack frame out of the hit path, as the production flat cache does. */
 EgtbSharedCache adapter={.backing=db,.slots=segment->slots,.words=segment->words,.words_per_page=db->memory_page_size/8};
 EgtbSharedCache *saved=p->cache;p->cache=&adapter;
 bool ok=shared_probe_miss(p,physical,slot,entry,side,value);p->cache=saved;return ok;
}
static bool __attribute__((noinline)) flat_get(EgtbSharedProbe *p,uint64_t index,EgtbSide side,int16_t *value) {
 return egtb_shared_probe_get(p,index,side,value);
}
/* Equal call boundaries: production callers are in a different translation
 * unit from egtb.c. Otherwise this include-based benchmark inlines only flat. */
static bool __attribute__((noinline)) seg_get(EgtbSharedProbe *p,uint64_t index,EgtbSide side,int16_t *value) {
 SegCache *c=(SegCache *)(void *)p->cache;Egtb *db=c->db;
 uint64_t page;uint32_t entry;split_entry_index(db,index,&page,&entry);
 size_t b=(page&(BUCKETS-1))+(size_t)side*BUCKETS;
 Bucket *q=&c->buckets[b];size_t ordinal=page/BUCKETS,local;
 if(c->linear) {
  size_t group=ordinal>>c->shift,seg=group&q->mask;
  if(seg>=q->count)seg-=q->base;
  local=(seg<<c->shift)|(ordinal&c->mask);
 }else local=cache_modulo(ordinal,q->modulo);
 Segment *segment=q->segments[local>>c->shift];size_t slot=local&c->mask;
 uint64_t physical=page+(uint64_t)side*db->side_page_count;
 size_t byte=(size_t)entry*2;SharedSlot *s=&segment->slots[slot];
 ++p->statistics.cache.lookups;
 uint64_t before=atomic_load_explicit(&s->sequence,memory_order_acquire);
 if(before&1)++p->statistics.busy_reads;
 else if(atomic_load_explicit(&s->tag,memory_order_relaxed)==physical) {
  uint64_t word=atomic_load_explicit(&segment->words[slot*(db->memory_page_size/8)+byte/8],memory_order_relaxed);
  atomic_thread_fence(memory_order_acquire);
  if(before==atomic_load_explicit(&s->sequence,memory_order_relaxed)) {
   int16_t stored;memcpy(&stored,(unsigned char*)&word+byte%8,2);*value=egtb_decode_dtm(stored);
   ++p->statistics.cache.hits;return true;
  }++p->statistics.invalidated_reads;
 }
 return seg_miss(p,segment,slot,physical,entry,side,value);
}

typedef struct {
 Egtb *db;EgtbResident *truth;EgtbSharedCache *flat;SegCache *seg;
 EgtbSharedProbe *probes[TEAM];
 uint64_t count,span,seed;int workload;bool validate;
 bool aligned;
 bool (*get)(EgtbSharedProbe *,uint64_t,EgtbSide,int16_t *);
 uint64_t sums[TEAM], hits[TEAM],misses[TEAM],decodes[TEAM],conflicts[TEAM];
} Run;
static uint64_t *trace_values[TEAM],trace_counts[TEAM];
static bool tracing;
static unsigned trace_repeats=1;
static uint64_t random64(uint64_t *s) {uint64_t x=*s;x^=x>>12;x^=x<<25;x^=x>>27;*s=x;return x*UINT64_C(2685821657736338717);}
static void probe_range(void *arg,unsigned id) {
 Run *r=arg;uint64_t state=r->seed+id*1299709+1,sum=0;
 EgtbSharedStatistics before,after;egtb_shared_probe_statistics(r->probes[id],&before);
 for(unsigned repeat=0;repeat<(tracing?trace_repeats:1);repeat++)
 for(uint64_t i=0;i<(tracing?trace_counts[id]:r->count);i++) {
  uint64_t index;EgtbSide side;int16_t value;
  if(tracing) {index=trace_values[id][i]>>1;side=(EgtbSide)(trace_values[id][i]&1);REQUIRE(index<=r->db->maximum_index);}
  else {
  uint64_t random=random64(&state),page;
  switch(r->workload) {
   case 0:page=random%(r->span/4);break; /* fits, uniform hot set */
   case 1:page=random%r->span;break;
   case 2:page=(random%10<9) ? ((random>>8)%(r->span/8)) : ((random>>8)%r->span);break;
   case 3:page=(i/16+(uint64_t)id*(r->span/TEAM))%r->span;break;
   case 4:page=(random%(r->span/BUCKETS))*BUCKETS;break; /* adversarial bucket skew */
   default:page=((i/16+(uint64_t)id*(r->span/TEAM))%r->span+r->span)%r->db->side_page_count;break;
  }
  index=page*r->db->entries_per_page+(random>>32)%r->db->entries_per_page;
  if(index>r->db->maximum_index)index=r->db->maximum_index;
  side=(EgtbSide)((random>>63)&1);
  }
  bool ok=r->get ? r->get(r->probes[id],index,side,&value) : r->seg ? seg_get(r->probes[id],index,side,&value) : flat_get(r->probes[id],index,side,&value);
  REQUIRE(ok);
  if(r->validate) {int16_t expected;REQUIRE(egtb_resident_get(r->truth,index,side,&expected));REQUIRE(value==expected);}
  sum+=(uint16_t)value;
 }
 r->sums[id]=sum;egtb_shared_probe_statistics(r->probes[id],&after);
 r->hits[id]=after.cache.hits-before.cache.hits;r->misses[id]=after.cache.misses-before.cache.misses;
 r->decodes[id]=after.cache.decompressions-before.cache.decompressions;
 r->conflicts[id]=after.publication_conflicts-before.publication_conflicts;
}
static void probes_open(Run *r) {
 r->aligned=getenv("SEG_ALIGN_PROBES") && atoi(getenv("SEG_ALIGN_PROBES"));
 for(unsigned i=0;i<TEAM;i++) {
  if(r->seg) {
   r->probes[i]=calloc(1,sizeof(EgtbSharedProbe));REQUIRE(r->probes[i]);
   REQUIRE(egtb_view_create(&r->probes[i]->scratch,r->db,1,false));
   /* Opaque benchmark context. Restore to NULL before the production destructor;
    * only seg_get/seg_miss interpret it while this probe is alive. */
   r->probes[i]->cache=(EgtbSharedCache *)(void *)r->seg;
  }else REQUIRE(egtb_shared_probe_create(&r->probes[i],r->flat));
  if(r->aligned) {
   EgtbSharedProbe *old=r->probes[i];
   EgtbSharedProbe *p=compat_aligned_alloc(64,(sizeof(*p)+63)&~(size_t)63);REQUIRE(p);memcpy(p,old,sizeof(*p));
   if(r->flat){REQUIRE(r->flat->probes==old);r->flat->probes=p;}
   free(old);r->probes[i]=p;
  }
 }
 /* Include both ends and the partial final page, even in prefix workloads. */
 for(unsigned end=0;end<2;end++)for(unsigned side=0;side<2;side++) {
  uint64_t index=end?r->db->maximum_index:0;int16_t value,expected;
  REQUIRE(r->seg?seg_get(r->probes[0],index,(EgtbSide)side,&value):flat_get(r->probes[0],index,(EgtbSide)side,&value));
  REQUIRE(egtb_resident_get(r->truth,index,(EgtbSide)side,&expected));REQUIRE(value==expected);
 }
}
static void probes_close(Run *r) {
 if(r->aligned) {
  if(r->flat)r->flat->probes=NULL;
  for(unsigned i=0;i<TEAM;i++){REQUIRE(egtb_view_close(r->probes[i]->scratch));compat_aligned_free(r->probes[i]);}
 }else for(unsigned i=0;i<TEAM;i++){if(r->seg)r->probes[i]->cache=NULL;egtb_shared_probe_destroy(r->probes[i]);}
}
static const char *seg_label(void) {return getenv("SEG_LINEAR") && atoi(getenv("SEG_LINEAR"))?"seg-linear":"seg-modulo";}
static double run(Run *r,const char *label,int round,const char *stage) {
 double t=shared_clock();team(probe_range,r);t=shared_clock()-t;
 uint64_t hits=0,misses=0,decodes=0,sum=0,conflicts=0;
 for(unsigned i=0;i<TEAM;i++){hits+=r->hits[i];misses+=r->misses[i];decodes+=r->decodes[i];sum+=r->sums[i];conflicts+=r->conflicts[i];}
 printf("RESULT,%s,%d,%s,%d,%.6f,%.3f,%.4f,%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",label,round,stage,r->workload,t,(hits+misses)/t/1e6,100.0*hits/(hits+misses),decodes,conflicts,sum);fflush(stdout);
 return t;
}
static void run_real(const char *path,size_t mib,size_t segmib,unsigned rounds,uint64_t count,bool validate) {
 Egtb *db;EgtbResident *truth=NULL;REQUIRE(egtb_open_readonly(&db,path,1));
 REQUIRE(db->planar);REQUIRE(egtb_resident_load(&truth,db,TEAM));
 printf("CONFIG,path=%s,positions=%" PRIu64 ",cache-MiB=%zu,segment-MiB=%zu,validate=%d\n",path,db->maximum_index+1,mib,segmib,validate);
 for(unsigned round=0;round<rounds;round++)for(int workload=0;workload<6;workload++)for(unsigned order=0;order<2;order++) {
  if(getenv("SEG_ONLY") && workload!=atoi(getenv("SEG_ONLY")))continue;
  bool segmented=(order^(round&1));size_t bytes=mib<<20;
  Run r={.db=db,.truth=truth,.count=count,.span=bytes/4096*2,.seed=43,.workload=workload,.validate=validate};
  if(r.span>db->side_page_count)r.span=db->side_page_count;
  if(segmented)r.seg=seg_create(db,bytes,segmib<<20);else REQUIRE(egtb_shared_cache_create_threads(&r.flat,db,bytes,TEAM));
  probes_open(&r);const char *label=segmented?seg_label():"flat";
  run(&r,label,round,"cold");run(&r,label,round,"warm");
  probes_close(&r);seg_destroy(r.seg);egtb_shared_cache_destroy(r.flat);
 }
 egtb_resident_destroy(truth);REQUIRE(egtb_close(db));
}
static void run_transfer(const char *path,const char *other,size_t mib,size_t segmib,unsigned rounds,uint64_t count) {
 Egtb *db,*db2;EgtbResident *truth,*truth2;REQUIRE(egtb_open_readonly(&db,path,1));REQUIRE(egtb_open_readonly(&db2,other,1));
 REQUIRE(egtb_resident_load(&truth,db,TEAM));REQUIRE(egtb_resident_load(&truth2,db2,TEAM));
 for(unsigned round=0;round<rounds;round++)for(unsigned order=0;order<3;order++) {
  unsigned mode=(round%2)?2-order:order;const char *label=mode==0?"discard":mode==1?"migrate":seg_label();
  size_t bytes=mib<<20,delta=bytes/4;Run a={.db=db,.truth=truth,.count=count,.span=bytes/4096,.seed=51,.workload=2,.validate=true};
  Run b={.db=db2,.truth=truth2,.count=count,.span=bytes/4096,.seed=91,.workload=2,.validate=true};
  if(getenv("SEG_WORKLOAD"))a.workload=b.workload=atoi(getenv("SEG_WORKLOAD"));
  if(a.span>db->side_page_count)a.span=db->side_page_count;if(b.span>db2->side_page_count)b.span=db2->side_page_count;
  if(mode==2){a.seg=seg_create(db,bytes,segmib<<20);b.seg=seg_create(db2,bytes/2,segmib<<20);}
  else{REQUIRE(egtb_shared_cache_create_threads(&a.flat,db,bytes,TEAM));REQUIRE(egtb_shared_cache_create_threads(&b.flat,db2,bytes/2,TEAM));}
  probes_open(&a);probes_open(&b);run(&a,label,round,"donor-warmup");run(&b,label,round,"receiver-warmup");
  double t=shared_clock();
  if(mode==2)seg_transfer(a.seg,b.seg,delta);
  else if(mode==1){REQUIRE(egtb_shared_cache_resize(a.flat,bytes-delta));REQUIRE(egtb_shared_cache_resize(b.flat,bytes/2+delta));}
  else{REQUIRE(egtb_shared_cache_discard_resize(a.flat,bytes-delta));REQUIRE(egtb_shared_cache_discard_resize(b.flat,bytes/2+delta));}
  printf("TRANSFER,%s,%u,forward,%.6f\n",label,round,shared_clock()-t);
  run(&a,label,round,"donor-after");run(&b,label,round,"receiver-after");
  t=shared_clock();
  if(mode==2)seg_transfer(b.seg,a.seg,delta);
  else if(mode==1){REQUIRE(egtb_shared_cache_resize(b.flat,bytes/2));REQUIRE(egtb_shared_cache_resize(a.flat,bytes));}
  else{REQUIRE(egtb_shared_cache_discard_resize(b.flat,bytes/2));REQUIRE(egtb_shared_cache_discard_resize(a.flat,bytes));}
  printf("TRANSFER,%s,%u,rollback,%.6f\n",label,round,shared_clock()-t);
  run(&a,label,round,"donor-rollback");run(&b,label,round,"receiver-rollback");
  probes_close(&a);probes_close(&b);seg_destroy(a.seg);seg_destroy(b.seg);egtb_shared_cache_destroy(a.flat);egtb_shared_cache_destroy(b.flat);
 }
 egtb_resident_destroy(truth);egtb_resident_destroy(truth2);REQUIRE(egtb_close(db));REQUIRE(egtb_close(db2));
}
typedef struct {EgtbSharedCache *flat; SegCache *seg;} Populate;
static void populate_range(void *arg,unsigned id) {
 Populate *p=arg;
 if(p->flat) {
  EgtbSharedCache *c=p->flat;
  for(size_t i=c->capacity*id/TEAM;i<c->capacity*(id+1)/TEAM;i++) {
   uint64_t tag=i%c->per_side+(i/c->per_side)*c->backing->side_page_count;
   atomic_store_explicit(&c->slots[i].tag,tag,memory_order_relaxed);
   atomic_store_explicit(&c->slots[i].sequence,2,memory_order_relaxed);
  }
 }else {
  SegCache *c=p->seg;
  for(size_t b=id;b<LANES;b+=TEAM)for(size_t n=0;n<c->buckets[b].count;n++)for(size_t i=0;i<c->slots;i++) {
   Segment *s=c->buckets[b].segments[n];
   uint64_t tag=(n*c->slots+i)*BUCKETS+b%BUCKETS+(b/BUCKETS)*c->db->side_page_count;
   atomic_store_explicit(&s->slots[i].tag,tag,memory_order_relaxed);
   atomic_store_explicit(&s->slots[i].sequence,2,memory_order_relaxed);
  }
 }
}
static void run_large(size_t mib,size_t segmib,unsigned rounds) {
 Egtb db={.readonly=true,.planar=true,.memory_page_size=2048,.side_page_count=UINT64_C(1000000000),.path="synthetic"};
 size_t bytes=mib<<20,receiver=bytes/4,delta=bytes/8;
 for(unsigned round=0;round<rounds;round++)for(unsigned order=0;order<3;order++) {
  unsigned mode=(round&1)?2-order:order;EgtbSharedCache *a=NULL,*b=NULL;SegCache *sa=NULL,*sb=NULL;
  const char *label=mode==0?"discard":mode==1?"migrate":seg_label();
  if(mode==2){sa=seg_create(&db,bytes,segmib<<20);sb=seg_create(&db,receiver,segmib<<20);}
  else{REQUIRE(egtb_shared_cache_create_threads(&a,&db,bytes,TEAM));REQUIRE(egtb_shared_cache_create_threads(&b,&db,receiver,TEAM));}
  Populate pa={a,sa},pb={b,sb};team(populate_range,&pa);team(populate_range,&pb);
  double t=shared_clock();
  if(mode==2)seg_transfer(sa,sb,delta);
  else if(mode==1){REQUIRE(egtb_shared_cache_resize(a,bytes-delta));REQUIRE(egtb_shared_cache_resize(b,receiver+delta));}
  else{REQUIRE(egtb_shared_cache_discard_resize(a,bytes-delta));REQUIRE(egtb_shared_cache_discard_resize(b,receiver+delta));}
  printf("LARGE,%s,%u,donor-MiB=%zu,receiver-MiB=%zu,transfer-MiB=%zu,segment-MiB=%zu,seconds=%.6f\n",label,round,mib,mib/4,mib/8,segmib,shared_clock()-t);fflush(stdout);
  seg_destroy(sa);seg_destroy(sb);egtb_shared_cache_destroy(a);egtb_shared_cache_destroy(b);
 }
}
int main(int argc,char **argv) {
 setvbuf(stdout,NULL,_IOLBF,0);
 if(getenv("SEG_TRACE")) {
  FILE *f=fopen(getenv("SEG_TRACE"),"rb");REQUIRE(f);
  REQUIRE(fread(trace_counts,sizeof(uint64_t),TEAM,f)==TEAM);
  for(unsigned i=0;i<TEAM;i++) {REQUIRE(trace_counts[i]<=1000000);trace_values[i]=malloc((trace_counts[i]+1)*sizeof(uint64_t));REQUIRE(trace_values[i]);REQUIRE(fread(trace_values[i],sizeof(uint64_t),trace_counts[i],f)==trace_counts[i]);}
  REQUIRE(!fclose(f));tracing=true;
  if(getenv("SEG_REPEATS"))trace_repeats=(unsigned)strtoul(getenv("SEG_REPEATS"),NULL,10);
  REQUIRE(trace_repeats>0 && trace_repeats<=100);
 }
 if(argc==8 && !strcmp(argv[1],"real"))run_real(argv[2],strtoul(argv[3],0,10),strtoul(argv[4],0,10),strtoul(argv[5],0,10),strtoull(argv[6],0,10),atoi(argv[7])!=0);
 else if(argc==8 && !strcmp(argv[1],"transfer"))run_transfer(argv[2],argv[3],strtoul(argv[4],0,10),strtoul(argv[5],0,10),strtoul(argv[6],0,10),strtoull(argv[7],0,10));
 else if(argc==5 && !strcmp(argv[1],"large"))run_large(strtoul(argv[2],0,10),strtoul(argv[3],0,10),strtoul(argv[4],0,10));
 else {fprintf(stderr,"real DB cacheMiB segmentMiB rounds lookups/thread validate\ntransfer DB1 DB2 cacheMiB segmentMiB rounds lookups/thread\nlarge donorMiB segmentMiB rounds\n");return 2;}
 for(unsigned i=0;i<TEAM;i++)free(trace_values[i]);return 0;
}
