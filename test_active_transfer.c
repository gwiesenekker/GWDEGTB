#define _POSIX_C_SOURCE 200809L
/* White-box fixtures set pre-trial policy measurements; all post-trial traffic
 * goes through real compressed pages and the public probe interface. */
#include "dependency_resident.c"
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
#ifdef EGTB_TEST_ALLOC_FAILURE
static unsigned fail_malloc_countdown;
void *__real_malloc(size_t);
void *__wrap_malloc(size_t size)
{
    if (fail_malloc_countdown && --fail_malloc_countdown==0) return NULL;
    return __real_malloc(size);
}
#endif

static void exercise(Egtb *a, Egtb *b, unsigned outcome)
{
    DependencyResidentPool *p;
    EgtbSharedCache *c[2]; EgtbSharedProbe *probe[2]; const EgtbResident *resident;
    CHECK(dependency_resident_create(&p,0,0));
    CHECK(dependency_shared_configure(p,4096,1024*1024));
    CHECK(dependency_shared_policy(p,"idle-reclaim-v1"));
    CHECK(dependency_resident_acquire(p,a,&resident));
    CHECK(dependency_shared_acquire(p,a,&c[0]));
    CHECK(dependency_resident_acquire(p,b,&resident));
    CHECK(dependency_shared_acquire(p,b,&c[1]));
    ResidentEntry *r=p->entries,*d=r->next;
    CHECK(r->shared==c[1] && d->shared==c[0]);
    CHECK(coordinated_resize(p,d,65536));
    uint64_t original=p->shared_allocated;
    uint64_t fallback=egtb_shared_cache_planned_allocation(a,256);
    p->shared_budget=original+fallback*2;
    for (unsigned k=0;k<2;++k) CHECK(egtb_shared_probe_create(&probe[k],c[k]));
    d->sampled=r->sampled=true;
    d->last_window_seconds=r->last_window_seconds=5;
    d->last_window_lookups=r->last_window_lookups=100001;
    d->window_lookups=r->window_lookups=100001;
    d->cost_samples=r->cost_samples=16;
    d->decode_ns=r->decode_ns=5000;
    d->baseline_rate=0; r->baseline_rate=20000;
    d->baseline_density=0; r->baseline_density=outcome==1 ? 1 : 1000000;
    uint64_t old[2]={egtb_shared_cache_bytes(c[0]),egtb_shared_cache_bytes(c[1])};
    if (outcome==4 || outcome==8) {
        if (outcome==4) p->shared_budget=original; /* No room for the safety net. */
        else p->resize_seconds_per_byte=1; /* Reject before any expensive mutation. */
        CHECK(!start_active_transfer(p,r,10));
        CHECK(!p->donor && p->shared_allocated==original);
    } else {
#ifdef EGTB_TEST_ALLOC_FAILURE
        if (outcome==5) fail_malloc_countdown=2; /* Donor replacement. */
        if (outcome==6) fail_malloc_countdown=4; /* Receiver replacement. */
#endif
        CHECK(start_active_transfer(p,r,10));
        if (outcome==5 || outcome==6) {
            CHECK(!p->active_trial && p->rollbacks==1);
            CHECK(p->shared_allocated==original && !p->recovery_reserve);
            CHECK(active_history(p,d,r)->failed);
            goto check_values;
        }
        CHECK(p->active_trial && p->transfers==1);
        CHECK(p->shared_allocated+p->recovery_reserve<=p->shared_budget);
        CHECK(old[0]-egtb_shared_cache_bytes(c[0])<=old[0]/20);
        CHECK(egtb_shared_cache_bytes(c[1])-old[1]<=old[1]/2);
        if (outcome==7) {
#ifdef EGTB_TEST_ALLOC_FAILURE
            fail_malloc_countdown=1;
#endif
            dependency_shared_phase_at(p,11);
            CHECK(p->active_trial && p->rollback_requested && p->recovery_reserve);
            CHECK(p->shared_allocated+p->recovery_reserve<=p->shared_budget);
            dependency_shared_maintain_at(p,12);
        } else if (outcome==2) dependency_shared_phase_at(p,11);
        else if (outcome==3) dependency_shared_maintain_at(p,191);
        else {
            for (unsigned w=0;w<3;++w) {
                for (unsigned j=0;j<100001;++j) {
                    int16_t value;
                    uint64_t donor_page=egtb_shared_cache_bytes(c[0])/256;
                    uint64_t i=outcome==1 && (j&1) ? donor_page*64 : 0;
                    CHECK(egtb_shared_probe_get(probe[0],i,EGTB_WHITE_TO_MOVE,&value));
                    CHECK(value==(int16_t)(i%2 ? 3 : -2));
                    i=(j&1) ? 1024 : 0;
                    CHECK(egtb_shared_probe_get(probe[1],i,EGTB_WHITE_TO_MOVE,&value));
                    CHECK(value==(int16_t)(i%2 ? 3 : -2));
                }
                dependency_shared_maintain_at(p,20+w*10);
                CHECK(p->shared_allocated+p->recovery_reserve<=p->shared_budget);
            }
        }
        CHECK(!p->active_trial && !p->donor && !p->recovery_reserve);
        CHECK(p->rollbacks==(outcome ? 1u : 0u));
        if (outcome) {
            CHECK(egtb_shared_cache_bytes(c[0])==old[0]);
            CHECK(egtb_shared_cache_bytes(c[1])==old[1]);
            ActiveHistory *history=active_history(p,d,r);
            CHECK(history && history->failed);
            CHECK(!active_retry_allowed(history,old[0],old[1],history->pressure,100000));
            dependency_shared_phase_at(p,100001);
            CHECK(!active_retry_allowed(history,old[0],old[1],history->pressure,100002));
            CHECK(!active_retry_allowed(history,old[0],old[1]*2,history->pressure,
                                       history->retry_after-1));
            CHECK(active_retry_allowed(history,old[0],old[1]*2,history->pressure,100003));
            CHECK(active_retry_allowed(history,old[0],old[1],history->pressure*2,100003));
            /* Reset sampling fixtures, but keep the failed-trial history: the
             * complete admission path must not resize again after cooldown. */
            d->sampled=r->sampled=true;
            d->window_lookups=r->window_lookups=100001;
            d->last_window_lookups=r->last_window_lookups=100001;
            d->last_window_seconds=r->last_window_seconds=5;
            d->cost_samples=r->cost_samples=16;
            d->baseline_rate=0; r->baseline_rate=20000;
            uint64_t allocation=p->shared_allocated;
            for (unsigned retry=0;retry<41;++retry)
                CHECK(!start_active_transfer(p,r,100010+retry*300));
            CHECK(p->shared_allocated==allocation && p->transfers==1);
        }
    }
check_values:
    for (unsigned k=0;k<2;++k) {
        for (uint64_t i=0;i<20000;++i) {
            int16_t value;
            CHECK(egtb_shared_probe_get(probe[k],i,EGTB_WHITE_TO_MOVE,&value));
            CHECK(value==(int16_t)(i%2 ? 3 : -2));
        }
        egtb_shared_probe_destroy(probe[k]);
    }
    dependency_resident_destroy(p);
}

int main(void)
{
    DependencyResidentPool budget={.workers=64};
    double seconds,savings;
    const uint64_t mib=1048576;
    /* Production-like 88 GiB donor, ~1 GiB transfer: avoid paying for a
     * huge resize even though the receiver is genuinely under pressure. */
    CHECK(!active_resize_affordable(&budget,90045*mib,89062*mib,4087*mib,5070*mib,
        4087*mib,983*mib,20,&seconds,&savings));
    CHECK(seconds>80 && savings>4 && savings<5);
    CHECK(active_resize_affordable(&budget,64*mib,61*mib,16*mib,19*mib,
        16*mib,3*mib,20,&seconds,&savings));
    budget.resize_seconds_per_byte=1e-6;
    CHECK(!active_resize_affordable(&budget,64*mib,61*mib,16*mib,19*mib,
        16*mib,3*mib,20,&seconds,&savings));
    char dir[]="/tmp/gwdegtb-active-XXXXXX",path[256],alias[256];
    CHECK(mkdtemp(dir));
    snprintf(path,sizeof(path),"%s/test.dtm",dir);
    Egtb *a,*b; EgtbCreateOptions options={4,20,1};
    CHECK(egtb_create(&a,path,19999,128,&options));
    for (uint64_t i=0;i<20000;++i) for (unsigned s=0;s<2;++s)
        CHECK(egtb_set(a,i,(EgtbSide)s,i%2 ? 3 : -2));
    CHECK(egtb_close(a));
    CHECK(egtb_open_readonly(&a,path,1));
    snprintf(alias,sizeof(alias),"%s/./test.dtm",dir);
    CHECK(egtb_open_readonly(&b,alias,1));
    for (unsigned i=0;i<5;++i) exercise(a,b,i);
    exercise(a,b,8);
#ifdef EGTB_TEST_ALLOC_FAILURE
    for (unsigned i=5;i<8;++i) exercise(a,b,i);
#endif
    CHECK(egtb_close(a)); CHECK(egtb_close(b));
    CHECK(unlink(path)==0); CHECK(rmdir(dir)==0);
    puts("active transfer: acceptance, donor harm, phase rollback, deadline, budget and values passed");
}
