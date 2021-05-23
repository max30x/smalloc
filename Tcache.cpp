#include "Tcache.hpp"

static atomic_bool arenas_inited = false;
static atomic_int inited_no = 0;
static atomic_int no = -1;
static arena_t arenas[CPUNUM_S];

static std::size_t bigchunk_size;
static int cache_num[NBINS+NLBINS];

static __thread bool tc_inited = false;
static __thread tcache_t tc;
static pthread_key_t pkey;

extern "C" {
    void* smalloc(std::size_t size);
    void* scalloc(std::size_t nitems,std::size_t size);
    void sfree(void* ptr);
}

void thread_cleanup(void* arg){
    for (int i=0;i<NBINS+NLBINS;++i){
        tbin_t* tbin = &tc.bins[i];
        for (int j=0;j<tbin->avail;++j){
            (i<NBINS) ? dalloc_small(tc.arena,tbin->ptrs[j])
                      : dalloc_large(tc.arena,tbin->ptrs[j]);
        }
    }
    dalloc_huge(tc.arena,(void*)tc.bins[0].ptrs);
    atomic_fetch_sub(&tc.arena->threads,1);
}

void arenas_cleanup(){
    if (tc_inited)
        thread_cleanup(nullptr);
    for (int i=0;i<CPUNUM_S;++i)
        clear_arena(&arenas[i]);
}

void* smalloc(std::size_t size){
    if (unlikely(!tc_inited)){
        pthread_key_create(&pkey,thread_cleanup);
        pthread_setspecific(pkey,&tc);  
        bool value = false;
        if (atomic_load(&no)==-1){
            int myturn = -1;
            while ((myturn=atomic_fetch_add(&inited_no,1))<CPUNUM_S)
                init_arena(&arenas[myturn]);
            if (atomic_compare_exchange_strong(&arenas_inited,&value,true)){
                atexit(arenas_cleanup);
                std::size_t _bigchunk_size = 0;
                for (int i=0;i<NBINS+NLBINS;++i){
                    int ncached = smax(CACHESIZE/regsize_to_bin[i],NCACHEDMIN);
                    cache_num[i] = ncached;
                    slog(LEVELA,"cache_num[%d]:%d\n",i,ncached);
                    _bigchunk_size += PTRSIZE*ncached;
                }
                bigchunk_size = NEXT_ALIGN(_bigchunk_size,CHUNKSIZE);
                slog(LEVELA,"bigchunk_size:%ld\n",bigchunk_size);
                atomic_store(&no,0);
            }
            else
                while (atomic_load(&no)==-1);
        }
        int myid = atomic_fetch_add(&no,1);
        myid %= CPUNUM_S;
        arena_t* arena = &arenas[myid];
        tc.arena = arena;
        atomic_fetch_add(&arena->threads,1);
        void** chunk = (void**)alloc_huge(arena,bigchunk_size);
        int ptrnum = 0;
        for (int i=0;i<NBINS+NLBINS;++i){
            tbin_t* tbin = &tc.bins[i];
            tbin->ratio = INITRATIO;
            tbin->ncached = cache_num[i];
            tbin->size = regsize_to_bin[i];
            tbin->ptrs = chunk+ptrnum;
            int alloc_num = smax(tbin->ncached>>tbin->ratio,1);
            (i>=NBINS) ? alloc_large_batch(tc.arena,tbin->size,tbin->ptrs,alloc_num)
                       : alloc_small_batch(tc.arena,tbin->size,tbin->ptrs,alloc_num);
            tbin->avail = alloc_num;
            ptrnum += tbin->ncached;
        }
        tc_inited = true;
    }
    int binid = size_class(size);
    if (likely(binid!=-1)){
        tbin_t* tbin = &tc.bins[binid];
        if (tbin->avail==0){
            int _ratio = (tbin->ratio!=0) ? --tbin->ratio : 0;
            int fillnum = tbin->ncached >> _ratio;
            (binid<NBINS) ? alloc_small_batch(tc.arena,tbin->size,tbin->ptrs,fillnum)
                          : alloc_large_batch(tc.arena,tbin->size,tbin->ptrs,fillnum);
            tbin->avail += fillnum;
            slog(LEVELB,"fill tbin(%d),fill %d items\n",binid,fillnum);
        }
        void* ret = tbin->ptrs[tbin->avail-1];
        --tbin->avail;
        return ret;
    }
    return alloc_huge(tc.arena,size);
}

void sfree(void* ptr){
    if (unlikely(ptr==nullptr))
        return;
    intptr_t _ptr = (intptr_t)ptr;
    if ((_ptr&(CHUNKSIZE-1))==0){
        // todo:need a better way to tell if this is a huge chunk
        dalloc_huge(tc.arena,ptr);
        return;
    }
    intptr_t chunkaddr = addr_to_chunk(_ptr);
    int pid = addr_to_pid(chunkaddr,_ptr);
    sbits* bs = pid_to_sbits(chunkaddr,pid);
    int binid = BINID(bs);
    tbin_t* tbin = &tc.bins[binid];
    if (unlikely(tbin->avail==tbin->ncached)){
        int type = TYPE(bs);
        int thrownum = (tbin->avail<(1<<2)) ? tbin->avail : tbin->avail>>2;
        for (int i=0;i<thrownum;++i){
            void* nowptr = tbin->ptrs[tbin->avail-1];
            (type==SMALL) ? dalloc_small(tc.arena,nowptr)
                          : dalloc_large(tc.arena,nowptr);
            --tbin->avail;
        }
        slog(LEVELB,"purge tbin(%d),throw %d items\n",binid,thrownum);
    }
    tbin->ptrs[tbin->avail++] = ptr;
}

void* scalloc(std::size_t nitems,std::size_t size){
    std::size_t _size = nitems*size;
    void* ptr = smalloc(_size);
    memset(ptr,0,_size);
    return ptr;
}