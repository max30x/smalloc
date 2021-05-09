#include "Arena.hpp"

static bool bigger_ad(const chunk_node_t* c1,const chunk_node_t* c2){
    return c1->start_addr > c2->start_addr;
}

static bool bigger_szad(const chunk_node_t* c1,const chunk_node_t* c2){
    if (c1->chunk_size==c2->chunk_size)
        return c1->start_addr > c2->start_addr;
    return c1->chunk_size > c2->chunk_size; 
}

static bool equal_ad(const chunk_node_t* c1,const chunk_node_t* c2){
    return c1->start_addr == c2->start_addr;
}

static bool equal_szad(const chunk_node_t* c1,const chunk_node_t* c2){
    return c1->chunk_size==c2->chunk_size && c1->start_addr==c2->start_addr;
}

static bool bigger_ad_span(const span_t* c1,const span_t* c2){
    return c1->start_pos > c2->start_pos;
}

static bool equal_ad_span(const span_t* c1,const span_t* c2){
    return c1->start_pos == c2->start_pos;
}

static bool bigger_szad_span(const span_t* c1,const span_t* c2){
    if (c1->spansize==c2->spansize)
        return c1->start_pos > c2->start_pos;
    return c1->spansize > c2->spansize;
}

static bool equal_szad_span(const span_t* c1,const span_t* c2){
    return c1->spansize==c2->spansize && c1->start_pos==c2->start_pos;
}

chunk_node_t* search_cnode(rb_tree_t<chunk_node_t>* tree,rbnode_t<chunk_node_t>* node){
    rbnode_t<chunk_node_t>* result = rb_search(tree,node);
    if (result==nullptr)
        return nullptr;
    return result->ptr;
}

void remove_cnode(rb_tree_t<chunk_node_t>* tree,rbnode_t<chunk_node_t>* node){
    rb_delete(tree,node);
}

void* chunk_alloc_slow(std::size_t size,int align,bool exec){
    std::size_t rsize = size + align;
    void* _addr = os_to_page(nullptr,rsize,exec);
    if (_addr==nullptr)
        return nullptr;
    intptr_t addr = (intptr_t)_addr;
    intptr_t ret = NEXT_ALIGN(addr,align);

    std::size_t lead = ret - addr;
    std::size_t tail = rsize - ret - size;
    if (lead!=0)
        page_to_os((void*)addr,lead);
    if (tail!=0)
        page_to_os((void*)(ret+size),tail);
    return (void*)ret;
}

void* chunk_alloc(std::size_t size,int align,std::size_t* tsize,bool exec){
    size = NEXT_ALIGN(size,CHUNKSIZE);
    if (tsize!=nullptr)
        *tsize = size;
    void* _addr = os_to_page(nullptr,size,exec);
    if (_addr==nullptr)
        return nullptr;
    intptr_t addr = (intptr_t)_addr;    
    if ((addr&(align-1)) != 0)
        return chunk_alloc_slow(size,align,exec);
    return (void*)addr;
}

void chunk_dalloc(void* ptr,std::size_t size){
    page_to_os(ptr,size);
}

void chunk_node_init(chunk_node_t* node,std::size_t size,intptr_t addr){
    node->chunk_size = size;
    node->start_addr = addr;
    lnode_init(&node->lchunk);
    lnode_init(&node->ldirty);
    rbnode_init(&node->anode,node,true);
    rbnode_init(&node->bnode,node,true);
}

bool alloc_chunk_for_node(mnode_t<chunk_node_t>* cnodes){
    std::size_t node_size = NEXT_ALIGN(sizeof(chunk_node_t),CACHELINE);
    cnodes->nsize = node_size;
    cnodes->offset = node_size;
    chunk_node_t* chunk = (chunk_node_t*)chunk_alloc(CHUNKSIZE,CACHELINE,nullptr,false);
    if (chunk==nullptr)
        return false;
    chunk_node_init(chunk,CHUNKSIZE,(intptr_t)chunk);
    cnodes->node_chunk = chunk;
    rb_insert(&cnodes->chunk_in_use,&chunk->anode);
    return true;
}

chunk_node_t* alloc_chunk_node(mnode_t<chunk_node_t>* cnodes){
    smutex_lock(&cnodes->mtx);
    chunk_node_t* fnodes = cnodes->node_free;
    if (fnodes!=nullptr){
        chunk_node_t* ret = fnodes;
        cnodes->node_free = *(chunk_node_t**)fnodes;
        smutex_unlock(&cnodes->mtx);
        return ret;
    }
    chunk_node_t* nchunk = cnodes->node_chunk;
    if (nchunk==nullptr || cnodes->offset+cnodes->nsize-1>nchunk->chunk_size)
        while (!alloc_chunk_for_node(cnodes));

    chunk_node_t* ret = (chunk_node_t*)(cnodes->node_chunk->start_addr+cnodes->offset);
    cnodes->offset += cnodes->nsize;
    smutex_unlock(&cnodes->mtx);
    return ret;
}

template<typename T>
void dalloc_node(mnode_t<T>* nodes,T* node){
    smutex_lock(&nodes->mtx);
    *(T**)node = nodes->node_free;
    nodes->node_free = node;
    smutex_unlock(&nodes->mtx);
}

template<typename T>
void nodes_init(mnode_t<T>* nodes){
    smutex_init(&nodes->mtx);
    rb_init(&nodes->chunk_in_use,bigger_ad,equal_ad);
    nodes->node_free = nullptr;
    nodes->node_chunk = nullptr;
    nodes->nsize = 0;
    nodes->offset = 0;
}

chunk_node_t* base_alloc(arena_t* arena,int align,std::size_t size,bool exec){
    std::size_t tsize;
    void* _addr = chunk_alloc(size,align,&tsize,exec);
    if (_addr==nullptr)
        return nullptr;

    arena->all_size += tsize;

    intptr_t addr = (intptr_t)_addr;
    chunk_node_t* cnode = alloc_chunk_node(&arena->chunk_nodes);
    chunk_node_init(cnode,tsize,addr);
    return cnode;
}

#ifdef SPECIAL
void base_dalloc(arena_t* arena,void* ptr){
    intptr_t addr = (intptr_t)ptr;
    chunk_node_t snode;
    chunk_node_init(&snode,0,addr);
    chunk_node_t* cnode = search_cnode(&arena->chunk_in_use,&snode.anode);
    remove_cnode(&arena->chunk_in_use,&cnode->anode);
    chunk_dalloc((void*)cnode->start_addr,cnode->chunk_size);
    arena->all_size -= cnode->chunk_size;
    dalloc_node(&arena->chunk_nodes,cnode);
}
#endif

void cleanup_map(rb_tree_t<chunk_node_t>* tree){
    if (rb_empty(tree))
        return;
    lnode_t sentry;
    lnode_init(&sentry);
    rb_iter_t<chunk_node_t> it(tree);
    while (true){
        chunk_node_t* now = it.next_ptr();
        if (now==nullptr)
            break;
        link_lnode(&sentry,&now->lchunk);
    }
    for (lnode_t* now=sentry.next;now!=&sentry;){
        lnode_t* next = now->next;
        chunk_node_t* _chunk = node_to_struct(chunk_node_t,lchunk,now);
        chunk_dalloc((void*)_chunk->start_addr,_chunk->chunk_size);
        now = next;
    }
}

void cleanup_arena(arena_t* arena){
    cleanup_map(&arena->chunk_cached_ad);
    cleanup_map(&arena->chunk_cached_szad);
    cleanup_map(&arena->chunk_in_use);
    cleanup_map(&arena->chunk_nodes.chunk_in_use);
}

void destroy_arena(arena_t* arena){
    while (atomic_load(&arena->threads)!=0);
    cleanup_arena(arena);
    std::size_t rsize = NEXT_ALIGN(sizeof(arena_t),CACHELINE);
    chunk_dalloc((void*)arena,rsize);
}

int rc_pagenum;
std::size_t rc_maxsize;
std::size_t rc_headersize;

// start from 0
int addr_to_pid(intptr_t chunkaddr,intptr_t addr){
    intptr_t content = chunkaddr + rc_headersize - 1;
    return NEXT_ALIGN(addr-content,PAGE)>>PAGESHIFT;
}

void init_spanbin(spanbin_t* bin,std::size_t regsize){
    smutex_init(&bin->mtx);
    bin->cur = nullptr;
    bin->spannum = 0;
    bin->span_not_full = 0;
    // todo : tune this
    int span_minsize = 16*1024;
    int _regnum = span_minsize/regsize;
    int regnum = max(REGMAX,_regnum);
    while (regnum*regsize>rc_maxsize)
        --regnum;
    bin->regnum = regnum;
    bin->regsize = regsize;
    bin->spansize = NEXT_ALIGN(regsize*regnum,PAGE);
    slog(LEVELA,"regnum:%d regsize:%lu spansize:%lu\n",regnum,regsize,bin->spansize);
}

void cal_rc_pagenum(std::size_t rcsize){
    std::size_t a = 0, b = rcsize;
    rc_pagenum = 0;
    for (;;){
        a += sizeof(sbits)+sizeof(span_t);
        b -= PAGE;
        if (a>b)
            break;
        ++rc_pagenum;
    }
    rc_maxsize = rc_pagenum*PAGE;
    rc_headersize = NEXT_ALIGN(rc_pagenum*(sizeof(span_t)+sizeof(sbits)),PAGE);
    slog(LEVELA,"rc_pagenum:%d rc_maxsize:%lu rc_headersize:%lu\n",rc_pagenum,rc_maxsize,rc_headersize);
}

// start from 1
span_t* pid_to_spanmeta(intptr_t chunkaddr,int pid){
    return (span_t*)(chunkaddr+(rc_pagenum<<SBITSSHIFT)+(pid-1)*sizeof(span_t));
}

// start from 1
sbits* pid_to_sbits(intptr_t chunkaddr,int pid){
    return (sbits*)(chunkaddr+((pid-1)<<SBITSSHIFT));
}

void link_dirty_arena(arena_t* arena,lnode_t* node){
    link_lnode(&arena->ldirty,node);
}

void unlink_dirty_arena(arena_t* arena,lnode_t* node){
    unlink_lnode(node);
}

void link_chunkdirty_arena(arena_t* arena,chunk_node_t* chunk){
    // why bother to link one chunk twice?
    // if when chunk and span are all represented as pointers
    // and can easily tell the difference between them,
    // then doing this is totally a waste of time
    link_lnode(&arena->lchunkdirty,&chunk->lchunk);
    link_lnode(&arena->ldirty,&chunk->ldirty);
}

void unlink_chunkdirty_arena(arena_t* arena,chunk_node_t* chunk){
    unlink_lnode(&chunk->lchunk);
    unlink_lnode(&chunk->ldirty);
}

void insert_map(arena_t* arena,chunk_node_t* chunk,bool dirty){
    if (dirty){
        rb_insert(&arena->chunk_dirty_szad,&chunk->anode);
        rb_insert(&arena->chunk_dirty_ad,&chunk->bnode);
        arena->dirty_size += chunk->chunk_size;
        link_chunkdirty_arena(arena,chunk);
        return;
    }
    rb_insert(&arena->chunk_cached_szad,&chunk->anode);
    rb_insert(&arena->chunk_cached_ad,&chunk->bnode);
}

void delete_map(arena_t* arena,chunk_node_t* chunk,bool dirty){
    if (dirty){
        rb_delete(&arena->chunk_dirty_szad,&chunk->anode);
        rb_delete(&arena->chunk_dirty_ad,&chunk->bnode);
        arena->dirty_size -= chunk->chunk_size;
        unlink_chunkdirty_arena(arena,chunk);
        return;
    }
    rb_delete(&arena->chunk_cached_szad,&chunk->anode);
    rb_delete(&arena->chunk_cached_ad,&chunk->bnode);
}

arena_t* new_arena(){
    std::size_t asize = NEXT_ALIGN(sizeof(arena_t),CACHELINE);
    std::size_t tsize;
    intptr_t addr = (intptr_t)chunk_alloc(asize,CHUNKSIZE,&tsize,false);
    arena_t* arena = (arena_t*)addr;
    
    arena->threads = 0;
    smutex_init(&arena->arena_mtx);
    rb_init(&arena->chunk_in_use,bigger_ad,equal_ad);

    nodes_init(&arena->chunk_nodes);
    
    arena->chunk_spared = nullptr;
    rb_init(&arena->chunk_cached_ad,bigger_ad,equal_ad);
    rb_init(&arena->chunk_cached_szad,bigger_szad,equal_szad);
    rb_init(&arena->chunk_dirty_ad,bigger_ad,equal_ad);
    rb_init(&arena->chunk_dirty_szad,bigger_szad,equal_szad);

    chunk_node_t* chunk = alloc_chunk_node(&arena->chunk_nodes);
    chunk_node_init(chunk,tsize-asize,addr+asize);
    insert_map(arena,chunk,false);

    rb_init(&arena->spanavail,bigger_szad_span,equal_szad_span);
    rb_init(&arena->spandirty,bigger_szad_span,equal_szad_span);

    cal_rc_pagenum(SPANCSIZE);

    lnode_init(&arena->ldirty);
    lnode_init(&arena->lchunkdirty);

    arena->all_size = 0;
    arena->dirty_size = 0;

    for (int i=0;i<NBINS;++i){
        spanbin_t* bin = &arena->bins[i];
        init_spanbin(bin,regsize_to_bin[i]);
        rb_init(&bin->spans,bigger_ad_span,equal_ad_span);
    }
    return arena;
}

chunk_node_t* chunk_from_map(arena_t* arena,std::size_t size,bool dirty){
    chunk_node_t* chunk = nullptr;
    rb_tree_t<chunk_node_t>* tree;
    if (dirty)
        tree = &arena->chunk_dirty_szad;
    else
        tree = &arena->chunk_cached_szad;
    
    rb_iter_t<chunk_node_t> it(tree);
    while (true){
        chunk = it.next_ptr();
        if (chunk==nullptr)
            break;
        if (chunk->chunk_size>=size){
            // iterator is invalid from here
            delete_map(arena,chunk,dirty);
            if (chunk->chunk_size==size){
                rb_insert(&arena->chunk_in_use,&chunk->anode);
                return chunk;
            }
            chunk_node_t* cnode = alloc_chunk_node(&arena->chunk_nodes);
            chunk_node_init(cnode,size,chunk->start_addr);
            rb_insert(&arena->chunk_in_use,&cnode->anode);

            chunk->start_addr += size;
            chunk->chunk_size -= size;
            insert_map(arena,chunk,dirty);
            return cnode;
        }
    }
    return nullptr;
}

bool behind_addr(chunk_node_t* c1,chunk_node_t* c2){
    return c1->start_addr < c2->start_addr;
}

bool mergable(chunk_node_t* c1,chunk_node_t* c2){
    return c1->start_addr+c1->chunk_size == c2->start_addr;
}

void chunk_to_map(arena_t* arena,chunk_node_t* chunk,bool dirty){
    rb_tree_t<chunk_node_t>* tree;
    if (dirty)
        tree = &arena->chunk_dirty_ad;
    else
        tree = &arena->chunk_cached_ad;
    
    rb_iter_t<chunk_node_t> it(tree);
    chunk_node_t *prev = nullptr, *now = nullptr;
    while (true){
        chunk_node_t* now = it.next_ptr();
        if (now==nullptr){
            if (prev==nullptr || !mergable(prev,chunk)){
                insert_map(arena,chunk,dirty);
                return;
            }
            // iterator is invalid from here
            delete_map(arena,prev,dirty);
            prev->chunk_size += chunk->chunk_size;
            dalloc_node(&arena->chunk_nodes,chunk);
            insert_map(arena,prev,dirty);
            return;
        }
        if (behind_addr(chunk,now)){
            if (!mergable(chunk,now) && prev==nullptr){
                insert_map(arena,chunk,dirty);
                return;
            }
            if (mergable(chunk,now)){
                // iterator is invalid from here
                delete_map(arena,now,dirty);
                chunk->chunk_size += now->chunk_size;
                dalloc_node(&arena->chunk_nodes,now);
            }
            if (prev!=nullptr && mergable(prev,chunk)){
                delete_map(arena,prev,dirty);
                prev->chunk_size += chunk->chunk_size;
                dalloc_node(&arena->chunk_nodes,chunk);
                chunk = prev;
            }
            insert_map(arena,chunk,dirty);
            return;
        }
        prev = now;
    }
}

void* new_chunk(arena_t* arena,std::size_t size){
    // the size below is so good looking
    // it's so big might even become a problem
    // but for now it isn't.Why...
    // so why would it end up like this?
    // because every chunk need to be aligned to CHUNKSIZE
    // and that's because the chunk used for span allocating must be aligned to a multiple of CHUNKSIZE
    // if break this,then chunks CAN NOT be merged
    size = NEXT_ALIGN(size,CHUNKSIZE);
    chunk_node_t* chunk = arena->chunk_spared;
    if (chunk!=nullptr && chunk->chunk_size>=size){
        arena->dirty_size -= size;
        if (chunk->chunk_size==size){
            arena->chunk_spared = nullptr;
            rb_insert(&arena->chunk_in_use,&chunk->anode);
            return (void*)chunk->start_addr;
        }
        chunk_node_t* cnode = alloc_chunk_node(&arena->chunk_nodes);
        chunk_node_init(cnode,size,chunk->start_addr);
        chunk->start_addr += size;
        chunk->chunk_size -= size;
        arena->chunk_spared = chunk;
        rb_insert(&arena->chunk_in_use,&cnode->anode);
        return (void*)cnode->start_addr;
    }

    chunk = chunk_from_map(arena,size,true);
    if (chunk!=nullptr)
        return (void*)chunk->start_addr;
        
    chunk = chunk_from_map(arena,size,false);
    if (chunk!=nullptr)
        return (void*)chunk->start_addr;
        
    chunk_node_t* cnode = base_alloc(arena,CHUNKSIZE,size,false);
    rb_insert(&arena->chunk_in_use,&cnode->anode);
    return (void*)cnode->start_addr;
}

void delete_chunk(arena_t* arena,void* addr,bool dirty){
    chunk_node_t snode;
    chunk_node_init(&snode,0,(intptr_t)addr);
    chunk_node_t* chunk = search_cnode(&arena->chunk_in_use,&snode.anode);
    // if under any circumstances,this function is called with dirty false.
    // then crash...
    rb_delete(&arena->chunk_in_use,&chunk->anode);
    if (!dirty){
        chunk_to_map(arena,chunk,dirty);
        return;
    }
    if (arena->chunk_spared==nullptr){
        arena->chunk_spared = chunk;
        arena->dirty_size += chunk->chunk_size;
        // need this. so link this.
        link_chunkdirty_arena(arena,chunk);
        return;
    }
    if (behind_addr(chunk,arena->chunk_spared) && mergable(chunk,arena->chunk_spared)){
        arena->chunk_spared->chunk_size += chunk->chunk_size;
        arena->chunk_spared->start_addr = chunk->start_addr;
        arena->dirty_size += chunk->chunk_size;
        dalloc_node(&arena->chunk_nodes,chunk);
        return;
    }
    else if (behind_addr(arena->chunk_spared,chunk) && mergable(arena->chunk_spared,chunk)){
        arena->chunk_spared->chunk_size += chunk->chunk_size;
        arena->dirty_size += chunk->chunk_size;
        dalloc_node(&arena->chunk_nodes,chunk);
        return;
    }
    if (behind_addr(chunk,arena->chunk_spared)){
        chunk_node_t* _chunk_spared = arena->chunk_spared;
        arena->dirty_size -= _chunk_spared->chunk_size;
        // don't need this. so unlink this.
        unlink_chunkdirty_arena(arena,_chunk_spared);
        arena->chunk_spared = chunk;
        arena->dirty_size += chunk->chunk_size;
        chunk_to_map(arena,_chunk_spared,dirty);
        return;
    }
    chunk_to_map(arena,chunk,dirty);
}

int size_class(std::size_t size){
    if (size>regsize_to_bin[NBINS+NLBINS-1])
        return -1;
    if (size<=8)
        return 0;
    if (size<=16)
        return 1;
    int ret = 0;
    if (size<=64){
        if ((size&0xf)!=0)
            ++ret;
        return ret + (size>>4);
    }
    int index = ff1_long(size);
    if ((size&((1<<(index-2))-1))!=0)
        ++ret;
    int diff = index - 6;
    ret += 4*diff;
    size >>= (diff+4);
    return ret + (size&3) + 4;
}

void init_span(span_t* span,std::size_t spansize,std::size_t regsize,int regnum){
    span->spansize = spansize;
    span->regsize = regsize;
    span->lfree = nullptr;
    span->nfree = 0;
    span->next_free = span->start_pos;
    span->regnum = regnum;
    lnode_init(&span->ldirty);
    rbnode_init(&span->anode,span,true);
}

void span_to_bin(span_t* span,spanbin_t* bin){
    ++bin->spannum;
    ++bin->span_not_full;
    if (bin->cur==nullptr){
        bin->cur = span;
        return;
    }
    rb_insert(&bin->spans,&span->anode);
}

void sbits_large(intptr_t start_pos,std::size_t size,int alloc,bool dirty,int binid){
    int _dirty = (dirty)?Y:N;
    intptr_t chunkaddr = addr_to_chunk(start_pos);
    int pid_head = addr_to_pid(chunkaddr,start_pos);
    int pid_tail = addr_to_pid(chunkaddr,start_pos+size-1);
    // make a big span means initialize the metadata of its head page and tail page
    sbits* bs_head = pid_to_sbits(chunkaddr,pid_head);
    SETALLOC(bs_head,alloc);
    SETPAGEID(bs_head,1);
    SETDIRTY(bs_head,_dirty);
    if (alloc==Y){
        SETTYPE(bs_head,LARGE);
        SETBINID(bs_head,binid);
    }
    if (pid_head==pid_tail)
        return;
    sbits* bs_tail = pid_to_sbits(chunkaddr,pid_tail);
    SETALLOC(bs_tail,alloc);
    SETPAGEID(bs_tail,pid_tail-pid_head+1);
    SETDIRTY(bs_tail,_dirty);
    if (alloc==Y){
        SETTYPE(bs_tail,LARGE);
        SETBINID(bs_tail,binid);
    }
}

void sbits_small_alloc(intptr_t start_pos,std::size_t size,int binid){
    intptr_t chunkaddr = addr_to_chunk(start_pos);
    int pid_head = addr_to_pid(chunkaddr,start_pos);
    int pid_tail = addr_to_pid(chunkaddr,start_pos+size-1);
    // slab,slab,slab is everywhere
    // so not one page is innocent
    for (int i=pid_head;i<=pid_tail;++i){
        sbits* bs = pid_to_sbits(chunkaddr,i);
        SETALLOC(bs,Y);
        SETBINID(bs,binid);
        SETPAGEID(bs,i+1-pid_head);
        SETTYPE(bs,SMALL);
        SETDIRTY(bs,Y);
    }
}

span_t* chunk_to_bigspan(void* chunkaddr){
    memset(chunkaddr,0,rc_headersize);
    intptr_t caddr = (intptr_t)chunkaddr;
    span_t* span_head = pid_to_spanmeta(caddr,1);
    span_head->spansize = rc_maxsize;
    span_head->start_pos = caddr+rc_headersize;
    rbnode_init(&span_head->anode,span_head,true);
    sbits_large(span_head->start_pos,rc_maxsize,N,false,-1);
    return span_head;
}

void insert_map_span(arena_t* arena,span_t* span,bool dirty){
    if (dirty){
        rb_insert(&arena->spandirty,&span->anode);
        link_dirty_arena(arena,&span->ldirty);
        arena->dirty_size += span->spansize;
        return;
    }
    rb_insert(&arena->spanavail,&span->anode);
}

void delete_map_span(arena_t* arena,span_t* span,bool dirty){
    if (dirty){
        rb_delete(&arena->spandirty,&span->anode);
        unlink_dirty_arena(arena,&span->ldirty);
        arena->dirty_size -= span->spansize;
        return;
    }
    rb_delete(&arena->spanavail,&span->anode);
}

// what i want is span
// what is returned is the unused one (so maybe i need to insert this into rbtree) 
span_t* split_bigspan(span_t* span,std::size_t size){
    if (span->spansize==size)
        return nullptr;
    intptr_t _spanaddr = span->start_pos+size;
    std::size_t _spansize = span->spansize-size;
    span->spansize = size;

    intptr_t chunkaddr = addr_to_chunk(_spanaddr);
    span_t* _span = pid_to_spanmeta(chunkaddr,addr_to_pid(chunkaddr,_spanaddr));
    _span->spansize = _spansize;
    _span->start_pos = _spanaddr;
    return _span;
}

span_t* span_from_map(arena_t* arena,std::size_t size,bool dirty){
    rb_tree_t<span_t>* tree;
    if (dirty)
        tree = &arena->spandirty;
    else
        tree = &arena->spanavail;
    rb_iter_t<span_t> it(tree);
    while (true){
        span_t* span = it.next_ptr();
        if (span==nullptr)
            break;
        if (span->spansize<size)
            continue;
        // iterator is invaild
        delete_map_span(arena,span,dirty);
        if (span->spansize==size)
            return span;
        span_t* left = split_bigspan(span,size);
        sbits_large(left->start_pos,left->spansize,N,dirty,-1);
        rbnode_init(&left->anode,left,true);
        insert_map_span(arena,left,dirty);
        return span;
    }
    return nullptr;
}

span_t* new_span(arena_t* arena,std::size_t size){
    span_t* span = span_from_map(arena,size,true);
    if (span!=nullptr)
        return span;

    span = span_from_map(arena,size,false);
    if (span!=nullptr)
        return span;
    
    void* ptr = new_chunk(arena,SPANCSIZE);
    span = chunk_to_bigspan(ptr);
    span_t* left = split_bigspan(span,size);
    sbits_large(span->start_pos,span->spansize,N,false,-1);
    if (left==nullptr)
        return span;
    sbits_large(left->start_pos,left->spansize,N,false,-1);
    rbnode_init(&left->anode,left,true);
    insert_map_span(arena,left,false);
    return span;
}

span_t* new_span_for_bin(arena_t* arena,int binid){
    spanbin_t* bin = &arena->bins[binid];
    std::size_t rsize = bin->spansize;
    
    smutex_lock(&arena->arena_mtx);
    span_t* span = new_span(arena,rsize);
    smutex_unlock(&arena->arena_mtx);
    if (span==nullptr)
        return nullptr;
    sbits_small_alloc(span->start_pos,rsize,binid);
    init_span(span,rsize,bin->regsize,bin->regnum);
    span_to_bin(span,bin);
    return span;
}

bool span_is_on_list(span_t* span){
    return span->next_free==span->start_pos+span->spansize;
}

bool span_is_full(span_t* span){
    return span->nfree==0 && span->next_free==span->start_pos+span->spansize;
}

void* alloc_reg(span_t* span){
    if (span->nfree>0){
        --span->nfree;
        lnode_t* ret = span->lfree;
        span->lfree = ret->next;
        return (void*)ret;
    }
    if (span_is_on_list(span))
        return nullptr;
    intptr_t ret = span->next_free;
    span->next_free += span->regsize;
    return (void*)ret;
}

void* alloc_small(arena_t* arena,std::size_t size){
    int binid = size_class(size);
    if (binid>=NBINS)
        return nullptr;
    spanbin_t* bin = &arena->bins[binid];
    smutex_lock(&bin->mtx);
    if (bin->spannum==0){
        if (new_span_for_bin(arena,binid)==nullptr){
            smutex_unlock(&bin->mtx);
            return nullptr;
        }       
    }
    span_t* span = nullptr;
    if (bin->cur!=nullptr && !span_is_full(bin->cur))
        span = bin->cur;
    else if (bin->span_not_full!=0){
        rb_iter_t<span_t> it(&bin->spans);
        while (true){
            span_t* s = it.next_ptr();
            if (s==nullptr)
                break;
            if (!span_is_full(s)){
                span = s;
                break;
            }
        }
        if (span!=nullptr){
            rb_delete(&bin->spans,&span->anode);
            if (bin->cur!=nullptr)
                rb_insert(&bin->spans,&bin->cur->anode);
            bin->cur = span;
        }
    }
    if (span==nullptr){
        span = new_span_for_bin(arena,binid);
        if (span==nullptr){
            smutex_unlock(&bin->mtx);
            return nullptr;
        }
    }
    void* ret = alloc_reg(span);
    if (span_is_full(span))
        --bin->span_not_full;
    smutex_unlock(&bin->mtx);
    return ret;
}

bool mergeable_span(span_t* a,span_t* b){  
    return a->start_pos+a->spansize == b->start_pos;
}

bool behind_addr_span(span_t* a,span_t* b){
    return a->start_pos < b->start_pos;
}

bool try_delete_span_chunk(arena_t* arena,span_t* span,bool dirty){
    if (span->spansize==rc_maxsize){
        delete_chunk(arena,(void*)(addr_to_chunk(span->start_pos)),dirty);
        return true;
    }
    return false;
}

void span_is_free(arena_t* arena,span_t* span,bool dirty){
    if (unlikely(try_delete_span_chunk(arena,span,dirty)))
        return;
    int d = (dirty) ? Y : N;
    intptr_t saddr = span->start_pos;
    intptr_t chunkaddr = addr_to_chunk(saddr);
    int pid = addr_to_pid(chunkaddr,saddr);
    if (pid>1){
        int prev_pid = pid-1;
        sbits* sb = pid_to_sbits(chunkaddr,prev_pid);
        if (ALLOC(sb)==N && DIRTY(sb)==d){ 
            int pageid = PAGEID(sb);
            prev_pid -= (pageid-1);
            span_t* prev = pid_to_spanmeta(chunkaddr,prev_pid);
            // todo:CAN NOT just simply change dirty size in method below
            delete_map_span(arena,prev,dirty);
            prev->spansize += span->spansize;
            span = prev;
            pid = prev_pid;
        }
    }
    if (pid<rc_pagenum){
        int span_pages = NEXT_ALIGN(span->spansize,PAGE)>>PAGESHIFT;
        int next_pid = pid+span_pages;
        if (next_pid<=rc_pagenum){
            sbits* sb = pid_to_sbits(chunkaddr,next_pid);
            if (ALLOC(sb)==N && DIRTY(sb)==d){
                span_t* next = pid_to_spanmeta(chunkaddr,next_pid);
                delete_map_span(arena,next,dirty);
                span->spansize += next->spansize;
            }
        }   
    }
    if (unlikely(try_delete_span_chunk(arena,span,dirty)))
        return;
    sbits_large(span->start_pos,span->spansize,N,dirty,-1);
    insert_map_span(arena,span,dirty);
}

std::size_t should_purge(arena_t* arena){
    if (arena->dirty_size==arena->all_size){
        // don't want to do purge immediately 
        // after freeing a huge chunk
        return 0;
    }
    std::size_t max_dirty_size = DIRTYMAX(arena->all_size);
    if (arena->dirty_size<max_dirty_size)
        return 0;
    return arena->dirty_size-max_dirty_size;
}

void purge(arena_t* arena,std::size_t size){
    lnode_t* now = arena->ldirty.next;
    lnode_t* cnow = arena->lchunkdirty.next;
    for (std::size_t fsize=0;now!=&arena->ldirty && fsize<size;){
        lnode_t* next_node = now->next;
        chunk_node_t* _chunk = nullptr;
        if (cnow!=&arena->lchunkdirty)
            _chunk = node_to_struct(chunk_node_t,lchunk,cnow);
        if (_chunk!=nullptr && now==&_chunk->ldirty){
            chunk_node_t* chunk = node_to_struct(chunk_node_t,ldirty,now);
            delete_map(arena,chunk,true);
            fsize += chunk->chunk_size;
            page_to_corner((void*)chunk->start_addr,chunk->chunk_size);
            chunk_to_map(arena,chunk,false);
            cnow = cnow->next;
        }
        else{
            span_t* span = node_to_struct(span_t,ldirty,now);
            delete_map_span(arena,span,true);
            fsize += span->spansize;
            page_to_corner((void*)span->start_pos,span->spansize);
            span_is_free(arena,span,false);
        }
        now = next_node;
    }
    if (now==&arena->ldirty){
        lnode_init(&arena->lchunkdirty);
        lnode_init(&arena->ldirty);
        return;
    }
    if (cnow==&arena->lchunkdirty)
        lnode_init(&arena->lchunkdirty);
    else
        arena->lchunkdirty.next = cnow;
    arena->ldirty.next = now;
}

void dalloc_small(arena_t* arena,void* ptr){
    intptr_t chunkaddr = addr_to_chunk((intptr_t)ptr);
    int pid = addr_to_pid(chunkaddr,(intptr_t)ptr);
    sbits* bs = pid_to_sbits(chunkaddr,pid);
    int binid = BINID(bs);
    int pageid = PAGEID(bs);
    pid -= (pageid-1);
    spanbin_t* bin = &arena->bins[binid];
    span_t* span = pid_to_spanmeta(chunkaddr,pid);

    smutex_lock(&bin->mtx);
    bool was_full = span_is_full(span);
    lnode_t* free_prev = span->lfree;
    span->lfree = (lnode_t*)ptr;
    span->lfree->next = free_prev;
    ++span->nfree;
    if (was_full)
        ++bin->span_not_full;
    if (span->nfree!=span->regnum || bin->spannum==1){
        smutex_unlock(&bin->mtx);
        return;
    }
    
    --bin->spannum;
    --bin->span_not_full;
    if (bin->cur==span)
        bin->cur = nullptr;
    else{
        rb_iter_t<span_t> it(&bin->spans);
        while (true){
            span_t* s = it.next_ptr();
            if (s==nullptr)
                break;
            if (span==s){
                rb_delete(&bin->spans,&s->anode);
                break;
            }
        }
    }
    smutex_unlock(&bin->mtx);

    smutex_lock(&arena->arena_mtx);
    span_is_free(arena,span,true);
    std::size_t wanted = should_purge(arena);
    if (unlikely(wanted>0))
        purge(arena,wanted);
    smutex_unlock(&arena->arena_mtx);
}

void* alloc_large(arena_t* arena,std::size_t size){
    int binid = size_class(size);
    size = regsize_to_bin[binid];
    smutex_lock(&arena->arena_mtx);
    span_t* span = new_span(arena,size);
    smutex_unlock(&arena->arena_mtx);
    lnode_init(&span->ldirty);
    sbits_large(span->start_pos,span->spansize,Y,true,binid);
    return (void*)span->start_pos;
}

void dalloc_large(arena_t* arena,void* ptr){
    intptr_t chunkaddr = addr_to_chunk((intptr_t)ptr);
    int pid = addr_to_pid(chunkaddr,(intptr_t)ptr);
    span_t* span = pid_to_spanmeta(chunkaddr,pid);
    smutex_lock(&arena->arena_mtx);
    span_is_free(arena,span,true);
    std::size_t wanted = should_purge(arena);
    if (unlikely(wanted>0))
        purge(arena,wanted);
    smutex_unlock(&arena->arena_mtx);
}

void* alloc_huge(arena_t* arena,std::size_t size){
    smutex_lock(&arena->arena_mtx);
    void* chunk = new_chunk(arena,size);
    smutex_unlock(&arena->arena_mtx);
    return chunk;
}

void dalloc_huge(arena_t* arena,void* ptr){
    smutex_lock(&arena->arena_mtx);
    delete_chunk(arena,ptr,true);
    std::size_t wanted = should_purge(arena);
    if (unlikely(wanted>0))
        purge(arena,wanted);
    smutex_unlock(&arena->arena_mtx);
}
