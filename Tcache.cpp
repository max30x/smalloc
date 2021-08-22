#include "Tcache.hpp"

static atomic_bool arenas_inited = false;
static atomic_int inited_no = 0;
static atomic_int no = -1;
static arena_t arenas[CPUNUM_S];

static tcaches_t tcs;

static std::size_t bigchunk_size;
static int cache_num[NBINS+NLBINS];

static __thread bool tc_inited = false;
static __thread tcache_t* tc;
static pthread_key_t pkey;

extern "C" {
    void* smalloc(std::size_t size);
    void sfree(void* ptr);
}

void init_tcaches(){
    smutex_init(&tcs.mtx,false);
    slnode_init(&tcs.mlink);
    slnode_init(&tcs.tlink);
    tcs.tcsize = sizeof(tcache_t);
    tcs.chunk = (char*)os_to_page(nullptr,CHUNKSIZE,false);
    tcs.limit = CHUNKSIZE;
    tcs.offset = 0;
}

tcache_t* new_tcache(){
    if (tcs.offset+tcs.tcsize>tcs.limit){
        slnode_t* chunk_node = (slnode_t*)tcs.chunk;
        chunk_node->next = tcs.mlink.next;
        tcs.mlink.next = chunk_node;
        tcs.chunk = (char*)os_to_page(nullptr,tcs.limit,false);
        tcs.offset = 0;
    }
    tcache_t* _tc = (tcache_t*)(tcs.chunk+tcs.offset);
    tcs.offset += tcs.tcsize;
    return _tc;
}

tcache_t* take_tcache(){
    if (tcs.tlink.next==nullptr)
        return nullptr;
    tcache_t* _tc = node_to_struct(tcache_t,tnode,tcs.tlink.next);
    tcs.tlink.next = _tc->tnode.next;
    m_at(_tc->arena!=nullptr,"arena should not be null\n");
    return _tc;
}

void return_tcache(tcache_t* _tc){
    _tc->tnode.next = tcs.tlink.next;
    tcs.tlink.next = &_tc->tnode;
}

void init_tcache(tcache_t* _tc,arena_t* _arena){
    slnode_init(&_tc->tnode);
    _tc->arena = _arena;
    void** chunk = (void**)alloc_huge(_arena,bigchunk_size);
    int ptrnum = 0;
    for (int i=0;i<NBINS+NLBINS;++i){
        tbin_t* tbin = &_tc->bins[i];
        tbin->ratio = INITRATIO;
        tbin->ncached = cache_num[i];
        tbin->size = regsize_to_bin[i];
        tbin->ptrs = chunk+ptrnum;
        int alloc_num = smax(tbin->ncached>>tbin->ratio,1);
        (i>=NBINS) ? alloc_large_batch(_tc->arena,i,tbin->ptrs,alloc_num)
                    : alloc_small_batch(_tc->arena,i,tbin->ptrs,alloc_num);
        tbin->avail = alloc_num;
        ptrnum += tbin->ncached;
    }
}

void thread_cleanup(void* arg){
    if (!tc_inited)
        return;
    tcache_t* _tc = (tcache_t*)arg;
    smutex_lock(&tcs.mtx);
    return_tcache(_tc);
    smutex_unlock(&tcs.mtx);
    tc = nullptr;
    tc_inited = false;
    atomic_fetch_sub(&_tc->arena->threads,1);
}

void arenas_cleanup(){
    if (tc_inited)
        thread_cleanup(tc);
    for (int i=0;i<CPUNUM_S;++i)
        clear_arena(&arenas[i]);
}

void tcaches_cleanup(){
    before_arena_destroy();
    arenas_cleanup();
    slnode_t* node = tcs.mlink.next;
    while (node!=nullptr){
        void* chunk = (void*)node;
        node = node->next;
        page_to_os(chunk,tcs.limit);
    }
}

void purge_bin(tbin_t* bin,int type,int thrownum){
    int thrownum_ = thrownum;
    void (*dfunc)(arena_t*,void*);
    if (type==SMALL)
        dfunc = dalloc_small;
    else
        dfunc = dalloc_large;

    int lastid = bin->avail-1;
    while (thrownum>0){
        intptr_t addr = (intptr_t)bin->ptrs[lastid];
        intptr_t chunk_addr = addr & ~(SPANCSIZE-1);
        chunk_node_t* chunk = (chunk_node_t*)chunk_addr;
        arena_t* arena = chunk->arena;
        bool first = true;
        if (type!=SMALL)
            smutex_lock(&arena->arena_mtx);
        for (int i=lastid;thrownum>0&&i>=0;--i){
            void* ptr = bin->ptrs[i];
            if (ptr==nullptr)
                continue;
            addr = (intptr_t)bin->ptrs[i];
            if (!ptr_in_chunk(chunk->start_addr,addr)){
                if (first){
                    lastid = i;
                    first = false;
                }
                continue;
            }
            dfunc(arena,(void*)addr);
            bin->ptrs[i] = nullptr;
            --thrownum;
        }
        if (type!=SMALL)
            smutex_unlock(&arena->arena_mtx);
    }
    if (thrownum_==bin->avail)
        return;
    int firstid = 0;
    while (bin->ptrs[firstid]==nullptr)
        ++firstid;
    int firstid_ = firstid;
    int left = bin->avail-thrownum_;
    while (left>0){
        if (bin->ptrs[firstid]==nullptr){
            int nextid = firstid+1;
            while (bin->ptrs[nextid]==nullptr)
                ++nextid;
            void* ptr_ = bin->ptrs[nextid];
            bin->ptrs[nextid] = nullptr;
            bin->ptrs[firstid] = ptr_;
        }
        ++firstid;
        --left;
    }
    if (firstid_==0)
        return;
    memcpy(bin->ptrs,bin->ptrs+firstid_,(bin->avail-thrownum_)*PTRSIZE);
}

void* smalloc(std::size_t size){
    if (unlikely(!tc_inited)){
        if (unlikely(atomic_load(&no)==-1)){
            bool value = false;
            if (atomic_compare_exchange_strong(&arenas_inited,&value,true)){
                before_arena_init();
                for (int i=0;i<CPUNUM_S;++i)
                    init_arena(&arenas[i]);
                std::size_t _bigchunk_size = 0;
                for (int i=0;i<NBINS+NLBINS;++i){
                    int ncached = smax(CACHESIZE/regsize_to_bin[i],NCACHEDMIN);
                    cache_num[i] = ncached;
                    slog(LEVELA,"cache_num[%d]:%d\n",i,ncached);
                    _bigchunk_size += PTRSIZE*ncached;
                }
                bigchunk_size = NEXT_ALIGN(_bigchunk_size,CHUNKSIZE);
                slog(LEVELA,"bigchunk_size:%ld\n",bigchunk_size);
                init_tcaches();
                atexit(tcaches_cleanup);
                atomic_store(&no,0);
            }
            else
                while (atomic_load(&no)==-1);
        }
        smutex_lock(&tcs.mtx);
        tc = take_tcache();
        if (tc==nullptr){
            tc = new_tcache();
            smutex_unlock(&tcs.mtx);
            //int myid = atomic_fetch_add(&no,1);
            //myid %= CPUNUM_S;
            int myid = 0;
            arena_t* arena = &arenas[myid];
            init_tcache(tc,arena);
        }else{
            smutex_unlock(&tcs.mtx);
        }
        atomic_fetch_add(&tc->arena->threads,1);
        pthread_key_create(&pkey,thread_cleanup);
        pthread_setspecific(pkey,tc);
        tc_inited = true;
    }
    int binid = size_class(size);
    if (likely(binid!=-1)){
        tbin_t* tbin = &tc->bins[binid];
        if (tbin->avail==0){
            int _ratio = (tbin->ratio!=0) ? --tbin->ratio : 0;
            int fillnum = tbin->ncached >> _ratio;
            (binid<NBINS) ? alloc_small_batch(tc->arena,binid,tbin->ptrs,fillnum)
                          : alloc_large_batch(tc->arena,binid,tbin->ptrs,fillnum);
            tbin->avail += fillnum;
            slog(LEVELB,"fill tbin(%d),fill %d items\n",binid,fillnum);
        }
        void* ret = tbin->ptrs[tbin->avail-1];
        --tbin->avail;
        return ret;
    }
    return alloc_huge(tc->arena,size);
}

void sfree(void* ptr){
    if (unlikely(ptr==nullptr))
        return;
    intptr_t _ptr = (intptr_t)ptr;
    if ((_ptr&(SPANCSIZE-1)) == 0){
        // todo:need a better way to tell if this is a huge chunk
        search_and_dalloc_huge(ptr);
        return;
    }
    intptr_t chunkaddr = addr_to_chunk_start(_ptr);
    chunkaddr = jump_to_sbit(chunkaddr);
    m_at(_ptr!=chunkaddr,"something wrong...\n");
    int pid = addr_to_pid(chunkaddr,_ptr);
    sbits* bs = pid_to_sbits(chunkaddr,pid);
    int binid = BINID(bs);
    tbin_t* tbin = &tc->bins[binid];
    if (unlikely(tbin->avail==tbin->ncached)){
        int type = (binid<NBINS) ? SMALL : LARGE;
        int thrownum = (tbin->avail<4) ? tbin->avail : tbin->avail>>2;
        purge_bin(tbin,type,thrownum);
        tbin->avail -= thrownum;
        slog(LEVELB,"purge tbin(%d),throw %d items\n",binid,thrownum);
    }
    tbin->ptrs[tbin->avail++] = ptr;
}