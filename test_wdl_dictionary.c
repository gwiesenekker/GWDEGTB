#define _POSIX_C_SOURCE 200809L
#include "wdl.h"
#include "gwdegtb.h"
#include "endgame_index.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s; %s; %s\n",__LINE__,#x,wdl_last_error(),gwdegtb_last_error());exit(1); } } while(0)
static const char *names[]={"2wX-0wO-2bX-0bO","2wX-0wO-1bX-1bO","1wX-0wO-1bX-0bO"};
static EgIndexer indexers[3];
static uint64_t maximum[3];
static unsigned char *images[3];
static size_t lengths[3];
static unsigned get32(const unsigned char *p){return (unsigned)p[0]|(unsigned)p[1]<<8|(unsigned)p[2]<<16|(unsigned)p[3]<<24;}
static uint64_t get64(const unsigned char *p){uint64_t n=0;for(unsigned i=0;i<8;i++)n|=(uint64_t)p[i]<<(8*i);return n;}
static unsigned result(unsigned db,uint64_t i,unsigned side){
    if ((i/2048)%7==0)return 0;
    uint64_t x=(i+1)*(UINT64_C(0x9e3779b97f4a7c15)+2*db);
    x^=x>>29;return (unsigned)((x+side+db)%3);
}
static void make_database(const char *dir,unsigned db){
    char source[256],output[256];size_t bytes;
    CHECK(gwdegtb_wdl_info(names[db],&maximum[db],&bytes));
    snprintf(source,sizeof source,"%s/%s.dtm",dir,names[db]);
    snprintf(output,sizeof output,"%s/%s.wdl",dir,names[db]);
    Egtb *dtm;EgtbCreateOptions opts={8,20,1};
    CHECK(egtb_create(&dtm,source,maximum[db],2048,&opts));
    for(uint64_t i=0;i<=maximum[db];i++){
        unsigned w=result(db,i,0),b=result(db,i,1);
        CHECK(egtb_set_pair(dtm,i,w==0?-1:w==1?3:-2,b==0?-1:b==1?5:-4));
    }
    CHECK(egtb_close(dtm));
    if(db==0){
        char plain[256];snprintf(plain,sizeof plain,"%s/plain.wdl",dir);
        CHECK(setenv("EGTB_WDL_DICTIONARY_KIB","invalid",1)==0);
        CHECK(!wdl_compile_threaded(source,plain,12,2,4,NULL,NULL));
        CHECK(setenv("EGTB_WDL_DICTIONARY_KIB","0",1)==0);
        CHECK(wdl_compile_threaded(source,plain,12,2,4,NULL,NULL));
        FILE *f=fopen(plain,"rb");CHECK(f);unsigned char header[64];
        CHECK(fread(header,1,sizeof header,f)==sizeof header);CHECK(fclose(f)==0);
        CHECK(header[8]==WDL_LEGACY_FORMAT_VERSION);CHECK(unlink(plain)==0);
        CHECK(unsetenv("EGTB_WDL_DICTIONARY_KIB")==0);
    }
    CHECK(wdl_compile_threaded(source,output,12,2,4,NULL,NULL));
    CHECK(wdl_file_size(output,&lengths[db]));images[db]=malloc(lengths[db]);CHECK(images[db]);
    CHECK(wdl_file_load_into(output,images[db],lengths[db]));
    CHECK(images[db][8]==(db<2?WDL_FORMAT_VERSION:WDL_LEGACY_FORMAT_VERSION));
    Wdl *w;CHECK(wdl_open(&w,output,1,12,2));
    unsigned char *bitmap=malloc(bytes);CHECK(bitmap);
    CHECK(wdl_decompress_into_threaded(w,bitmap,bytes,4));
    for(uint64_t i=0;i<=maximum[db];i++)for(unsigned side=0;side<2;side++)
        CHECK(((bitmap[i/2]>>((i%2)*4+side*2))&3)==result(db,i,side));
    for(uint64_t i=0;i<=maximum[db];i+=997){WdlResult got;CHECK(wdl_get(w,i,EGTB_WHITE_TO_MOVE,&got));CHECK((unsigned)got==result(db,i,0));}
    CHECK(wdl_close(w));free(bitmap);
    if(db<2){
        size_t dict_size=get32(images[db]+56);
        size_t dict_start=(size_t)get64(images[db]+40)-dict_size;
        CHECK(dict_size&&dict_start+dict_size<=lengths[db]);
        WdlImage *bad=NULL;
        images[db][dict_start]^=1;
        CHECK(!wdl_image_attach(&bad,images[db],lengths[db]));
        CHECK(strstr(wdl_last_error(),"dictionary"));
        FILE *f=fopen(output,"r+b");CHECK(f);CHECK(fseeko(f,(off_t)dict_start,SEEK_SET)==0);
        CHECK(fputc(images[db][dict_start],f)!=EOF);CHECK(fclose(f)==0);
        CHECK(!wdl_open(&w,output,1,12,2)); /* disk reader rejects bad CRC too */
        images[db][dict_start]^=1;
        CHECK(!wdl_image_attach(&bad,images[db],dict_start+dict_size-1));
        unsigned char old=images[db][59];images[db][59]=255;
        CHECK(!wdl_image_attach(&bad,images[db],lengths[db]));images[db][59]=old;
    }
    CHECK(gwdegtb_wdl_compressed_attach(names[db],images[db],lengths[db]));
    CHECK(unlink(source)==0);CHECK(unlink(output)==0);
}
static void *worker(void *arg){
    uint64_t rng=(uintptr_t)arg+1;GwdegtbWdlProbe *p;CHECK(gwdegtb_wdl_probe_create(4096,&p));
    for(unsigned q=0;q<30000;q++){
        rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;
        unsigned db=q%3,side=(unsigned)(rng&1);uint64_t i=rng%(maximum[db]+1);EgPosition b;
        CHECK(eg_index_to_position(&indexers[db],i,&b));
        int got=gwdegtb_wdl_lookup_probe_compact(p,b.white_kings,b.white_men,b.black_kings,b.black_men,(GwdegtbSide)side);
        unsigned want=result(db,i,side);CHECK(got==(want==0?-1:want==1?1:0));
    }
    gwdegtb_wdl_probe_destroy(p);return NULL;
}
int main(void){
    char dir[]="/tmp/gwdegtb-wdl-dictionary-XXXXXX";CHECK(mkdtemp(dir));
    CHECK(unsetenv("EGTB_WDL_DICTIONARY_KIB")==0);
    CHECK(eg_indexer_init(&indexers[0],0,0,2,2));
    CHECK(eg_indexer_init(&indexers[1],0,1,2,1));
    CHECK(eg_indexer_init(&indexers[2],0,0,1,1));
    for(unsigned i=0;i<3;i++)make_database(dir,i);
    pthread_t t[4];for(uintptr_t i=0;i<4;i++)CHECK(pthread_create(&t[i],NULL,worker,(void*)i)==0);
    for(unsigned i=0;i<4;i++)CHECK(pthread_join(t[i],NULL)==0);
    gwdegtb_wdl_compressed_unload_all();
    for(unsigned i=0;i<3;i++){free(images[i]);eg_indexer_destroy(&indexers[i]);}
    CHECK(rmdir(dir)==0);puts("WDL dictionaries: legacy, corruption, resident, mixed threaded probes PASS");return 0;
}
