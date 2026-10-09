/* Optional trace-capture storage object for an isolated benchmark generator.
 * Build instead of egtb.o; never use this object in production. Captures the
 * first million target-database queries of each of the first 16 calling threads. */
#define egtb_shared_probe_get trace_original_get
#include "egtb.c"
#undef egtb_shared_probe_get
#define TRACE_THREADS 16
#define TRACE_CAPACITY 1000000
static _Atomic unsigned trace_next;
static uint64_t *trace_values[TRACE_THREADS],trace_counts[TRACE_THREADS];
static _Thread_local unsigned trace_id;
static _Thread_local bool trace_assigned;
static void trace_save(void) {
 const char *path=getenv("BENCH_TRACE");if(!path)return;
 FILE *f=fopen(path,"wb");if(!f)abort();
 if(fwrite(trace_counts,sizeof(uint64_t),TRACE_THREADS,f)!=TRACE_THREADS)abort();
 for(unsigned i=0;i<TRACE_THREADS;i++) {
  if(trace_counts[i] && fwrite(trace_values[i],sizeof(uint64_t),trace_counts[i],f)!=trace_counts[i])abort();
  free(trace_values[i]);
 }
 if(fclose(f))abort();
}
bool egtb_shared_probe_get(EgtbSharedProbe *p,uint64_t index,EgtbSide side,int16_t *value) {
 const char *target=getenv("BENCH_TARGET");const char *name=strrchr(p->cache->backing->path,'/');name=name?name+1:p->cache->backing->path;
 if(target && !strcmp(name,target)) {
  if(!trace_assigned) {
   trace_id=atomic_fetch_add_explicit(&trace_next,1,memory_order_relaxed);trace_assigned=true;
   if(!trace_id && atexit(trace_save))abort();
   if(trace_id<TRACE_THREADS){trace_values[trace_id]=malloc(TRACE_CAPACITY*sizeof(uint64_t));if(!trace_values[trace_id])abort();}
  }
  if(trace_id<TRACE_THREADS && trace_counts[trace_id]<TRACE_CAPACITY)
   trace_values[trace_id][trace_counts[trace_id]++]=(index<<1)|(unsigned)side;
 }
 return trace_original_get(p,index,side,value);
}
