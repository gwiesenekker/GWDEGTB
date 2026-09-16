/* Offline experiment on exact on-disk codec streams; does not modify EGTBs.
 * Build: cc -O3 -DNDEBUG -march=native -std=c11 -pthread
 *        benchmark_zstd_dictionary.c -lzstd -o /tmp/bench-zdict
 * Run: /tmp/bench-zdict FILE.dtm (format 4) or FILE.wdl (format 1 or 2).
 */
#define _POSIX_C_SOURCE 200809L
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#include <zdict.h>
#include "crc32c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>

#define TRAIN 4000u
#define TEST 10000u
#define ROUNDS 5u
#define CAP 3072u
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"failed line %d: %s\n",__LINE__,#x);exit(1); } } while(0)
typedef struct { unsigned char raw[CAP]; size_t size; uint32_t crc; } Page;
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
static uint64_t get64(const unsigned char *p){uint64_t v=0;for(unsigned i=0;i<8;i++)v|=(uint64_t)p[i]<<(8*i);return v;}
static uint32_t get32(const unsigned char *p){uint32_t v=0;for(unsigned i=0;i<4;i++)v|=(uint32_t)p[i]<<(8*i);return v;}
static unsigned get16(const unsigned char *p){return p[0]|(unsigned)p[1]<<8;}
static uint64_t rng=0x6a09e667f3bcc909ULL;
static uint64_t rnd(void){rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return rng;}
/* Match format-4 escape expansion and CRC over canonical 16-bit codes. */
static uint32_t decoded_crc(const unsigned char *p,size_t n,int dtm){
 if(!dtm)return crc32c(p,n);
 unsigned char canonical[2048];size_t at=0;
 for(size_t i=0;i<1024;i++){
  CHECK(at<n);unsigned b=p[at++];int v;
  if(b==128)v=INT16_MIN;
  else if(b==127){CHECK(at+2<=n);unsigned r=get16(p+at);at+=2;v=r<=INT16_MAX?(int)r:(int)r-65536;CHECK(v!=INT16_MIN&&(v < -127 || v >126));}
  else v=b<128?(int)b:(int)b-256;
  CHECK(v==INT16_MIN||(v>=-16383&&v<=16383));
  canonical[2*i]=(unsigned char)v;canonical[2*i+1]=(unsigned char)((unsigned)v>>8);
 }
 CHECK(at==n);return crc32c(canonical,sizeof canonical);
}
static double median(double *a){for(unsigned i=0;i<ROUNDS;i++)for(unsigned j=i+1;j<ROUNDS;j++)if(a[j]<a[i]){double t=a[i];a[i]=a[j];a[j]=t;}return a[ROUNDS/2];}
static void bench(Page *pages,const unsigned char *dict,size_t dict_size,
                  int level,int no_literals,int dtm){
 ZSTD_CCtx *cc=ZSTD_createCCtx();ZSTD_DCtx *dc=ZSTD_createDCtx();CHECK(cc&&dc);
 ZSTD_CDict *cd=NULL;ZSTD_DDict *dd=NULL;double setup=now();
 if(dict_size){cd=ZSTD_createCDict(dict,dict_size,level);dd=ZSTD_createDDict(dict,dict_size);CHECK(cd&&dd);}
 setup=now()-setup;
 CHECK(!ZSTD_isError(ZSTD_CCtx_setParameter(cc,ZSTD_c_compressionLevel,level)));
 if(cd)CHECK(!ZSTD_isError(ZSTD_CCtx_refCDict(cc,cd)));
 if(no_literals)CHECK(!ZSTD_isError(ZSTD_CCtx_setParameter(cc,ZSTD_c_literalCompressionMode,ZSTD_lcm_uncompressed)));
 CHECK(!ZSTD_isError(ZSTD_DCtx_refDDict(dc,dd)));
 size_t bound=ZSTD_compressBound(CAP),*lengths=malloc(TEST*sizeof(*lengths));
 unsigned char *compressed=malloc(TEST*bound),out[CAP];CHECK(lengths&&compressed);
 uint64_t raw_bytes=0,packed_bytes=0;double t=now();
 for(unsigned i=0;i<TEST;i++){
  lengths[i]=ZSTD_compress2(cc,compressed+i*bound,bound,pages[i].raw,pages[i].size);
  CHECK(!ZSTD_isError(lengths[i]));packed_bytes+=lengths[i];raw_bytes+=pages[i].size;
 }
 double enc=now()-t;
 /* Untimed warm-up: exact-byte validation, plus existing on-disk CRC. */
 for(unsigned i=0;i<TEST;i++){
  size_t n=ZSTD_decompressDCtx(dc,out,sizeof out,compressed+i*bound,lengths[i]);
  CHECK(n==pages[i].size&&memcmp(out,pages[i].raw,n)==0);
  CHECK(decoded_crc(out,n,dtm)==pages[i].crc);
 }
 double pure[ROUNDS],complete[ROUNDS];
 for(unsigned r=0;r<ROUNDS;r++){
  t=now();for(unsigned i=0;i<TEST;i++){size_t n=ZSTD_decompressDCtx(dc,out,sizeof out,compressed+i*bound,lengths[i]);CHECK(n==pages[i].size);}pure[r]=now()-t;
  t=now();for(unsigned i=0;i<TEST;i++){size_t n=ZSTD_decompressDCtx(dc,out,sizeof out,compressed+i*bound,lengths[i]);CHECK(n==pages[i].size);CHECK(decoded_crc(out,n,dtm)==pages[i].crc);}complete[r]=now()-t;
 }
 double p=median(pure),c=median(complete);
 printf("result,%d,%zu,%d,%llu,%llu,%.4f,%.4f,%.4f,%.4f,%.4f,%.6f\n",level,dict_size,no_literals,(unsigned long long)raw_bytes,(unsigned long long)packed_bytes,enc*1e6/TEST,p*1e6/TEST,c*1e6/TEST,pure[0]*1e6/TEST,complete[0]*1e6/TEST,setup);
 free(lengths);free(compressed);ZSTD_freeCCtx(cc);ZSTD_freeDCtx(dc);ZSTD_freeCDict(cd);ZSTD_freeDDict(dd);
}
int main(int argc,char **argv){
 CHECK(argc==2);setvbuf(stdout,NULL,_IOLBF,0);
 int dtm=strstr(argv[1],".dtm")!=NULL;FILE *f=fopen(argv[1],"rb");CHECK(f);
 CHECK(fseeko(f,0,SEEK_END)==0);off_t file_size=ftello(f);CHECK(file_size>64);rewind(f);
 unsigned char *file=malloc((size_t)file_size);CHECK(file);CHECK(fread(file,1,(size_t)file_size,f)==(size_t)file_size);fclose(f);
 CHECK(dtm?file[8]==4:(file[8]==1||file[8]==2));CHECK(get32(file+12)==(dtm?2048:1024));
 uint64_t count=get64(file+24),directory=get64(file+32),used=0;
 uint64_t *ids=malloc((size_t)count*sizeof(*ids));CHECK(ids);
 size_t stride=dtm?10:14;CHECK(directory+count*stride<=(uint64_t)file_size);
 for(uint64_t i=0;i<count;i++)if(get64(file+directory+i*stride))ids[used++]=i;
 CHECK(used>=TRAIN+TEST);
 for(uint64_t i=used-1;i>0;i--){uint64_t j=rnd()%(i+1),t=ids[i];ids[i]=ids[j];ids[j]=t;}
 Page *pages=calloc(TRAIN+TEST,sizeof(*pages));size_t *training_sizes=malloc(TRAIN*sizeof(*training_sizes));unsigned char *training=malloc(TRAIN*CAP);CHECK(pages&&training_sizes&&training);
 ZSTD_DCtx *dc=ZSTD_createDCtx();CHECK(dc);size_t train_bytes=0;ZSTD_DDict *source_dict=NULL;
 if(!dtm&&file[8]==2){
  size_t n=get32(file+56);uint64_t end=get64(file+40);
  CHECK(n&&n<=256*1024&&end<=(uint64_t)file_size&&end>=n);
  CHECK(crc32c(file+end-n,n)==get32(file+60));
  source_dict=ZSTD_createDDict(file+end-n,n);CHECK(source_dict);
  CHECK(!ZSTD_isError(ZSTD_DCtx_refDDict(dc,source_dict)));
 }
 for(unsigned i=0;i<TRAIN+TEST;i++){
  const unsigned char *e=file+directory+ids[i]*stride;uint64_t off=get64(e);size_t n=get16(e+8);CHECK(off+n+(dtm?4:0)<=(uint64_t)file_size);
  pages[i].crc=dtm?get32(file+off):get32(e+10);
  pages[i].size=ZSTD_decompressDCtx(dc,pages[i].raw,CAP,file+off+(dtm?4:0),n);CHECK(!ZSTD_isError(pages[i].size));CHECK(decoded_crc(pages[i].raw,pages[i].size,dtm)==pages[i].crc);
  if(i<TRAIN){training_sizes[i]=pages[i].size;memcpy(training+train_bytes,pages[i].raw,pages[i].size);train_bytes+=pages[i].size;}
 }
 printf("source,%s,pages,%llu,stored,%llu,train,%u,test,%u,zstd,%s\n",argv[1],(unsigned long long)count,(unsigned long long)used,TRAIN,TEST,ZSTD_versionString());
 puts("columns,level,dict_bytes,literals_off,codec_bytes,payload_bytes,compress_us,pure_decode_median_us,codec_crc_median_us,pure_best_us,codec_crc_best_us,prepared_dict_seconds");
 free(file);free(ids);ZSTD_freeDCtx(dc);ZSTD_freeDDict(source_dict);
 int levels[]={1,3,6,8,9,12,19};for(unsigned i=0;i<sizeof(levels)/sizeof(*levels);i++)bench(pages+TRAIN,NULL,0,levels[i],0,dtm);
 bench(pages+TRAIN,NULL,0,6,1,dtm);
 size_t caps[]={16*1024,64*1024,110*1024};int dl[]={3,6,9,12};
 for(unsigned d=0;d<3;d++){
  unsigned char *dict=malloc(caps[d]);CHECK(dict);double t=now();size_t n=ZDICT_trainFromBuffer(dict,caps[d],training,training_sizes,TRAIN);CHECK(!ZDICT_isError(n));
  printf("training,%zu,%zu,%.6f\n",caps[d],n,now()-t);
  for(unsigned l=0;l<4;l++)bench(pages+TRAIN,dict,n,dl[l],0,dtm);
  free(dict);
 }
 free(pages);free(training);free(training_sizes);puts("PASS: exact codec bytes and all source-page checksums verified");return 0;
}
