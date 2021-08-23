#include "Arena.hpp"

static int sc_pagenum;
static std::size_t sc_maxsize;
static std::size_t sc_headersize;

static smutex_t hcs_mtx;
static rb_tree_t<chunk_node_t> huge_chunks;

chunk_node_t* make_chunknode(arena_t* arena,intptr_t raw_chunk,bool huge);

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
    return (chunk_node_t*)result->ptr;
}

chunk_node_t* fsearch_cnode(rb_tree_t<chunk_node_t>* tree,chunk_node_t* kchunk){
    rbnode_t<chunk_node_t>* result = rb_fsearch(tree,&kchunk->anode);
    if (result==nullptr)
        return nullptr;
    return (chunk_node_t*)result->ptr;
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
    if ((addr&(align-1)) != 0){
        page_to_os(_addr,size);
        return chunk_alloc_slow(size,align,exec);
    }
    return (void*)addr;
}

void chunk_dalloc(void* ptr,std::size_t size){
    page_to_os(ptr,size);
}

void chunk_node_init(chunk_node_t* node,arena_t* arena,std::size_t size,intptr_t addr,bool embed){
    m_at(is_alignof(addr,CHUNKSIZE),"addr is not a mutiple of CHUNKSIZE\n");
    m_at(is_multipleof(size,CHUNKSIZE),"size is not a multiple of CHUNKSIZE");
    node->arena = arena;
    if (embed) {
        size -= CHUNKHEADER;
        addr += CHUNKHEADER;
    }
    node->chunk_size = size;
    node->start_addr = addr;
    lnode_init(&node->lchunk);
    lnode_init(&node->ldirty);
    rbnode_init(&node->anode,node,true);
    rbnode_init(&node->bnode,node,true);
}

bool alloc_chunk_for_node(mnode_t<chunk_node_t>* cnodes){
    std::size_t node_size = sizeof(chunk_node_t);
    cnodes->nsize = node_size;
    cnodes->offset = node_size;
    chunk_node_t* chunk = (chunk_node_t*)chunk_alloc(CHUNKSIZE,CHUNKSIZE,nullptr,false);
    if (chunk==nullptr)
        return false;

    // initializing chunk node like this is a special case
    // might become a problem if don't handle this properly
    chunk_node_init(chunk,nullptr,CHUNKSIZE,(intptr_t)chunk,false);
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
    smutex_init(&nodes->mtx,false);
    rb_init(&nodes->chunk_in_use,bigger_ad,equal_ad);
    nodes->node_free = nullptr;
    nodes->node_chunk = nullptr;
    nodes->nsize = 0;
    nodes->offset = 0;
}

chunk_node_t* base_alloc(arena_t* arena,int align,std::size_t size,bool exec,bool huge){
    std::size_t tsize;
    void* _addr = chunk_alloc(size,align,&tsize,exec);
    if (_addr==nullptr)
        return nullptr;

    m_at(is_alignof((intptr_t)_addr,CHUNKSIZE),"addr is not aligned\n");

    arena->all_size += tsize;
    intptr_t addr = (intptr_t)_addr;
    chunk_node_t* cnode = make_chunknode(arena,addr,huge);
    chunk_node_init(cnode,arena,tsize,addr,ISEMBED(huge));
    return cnode;
}

#if 0
void base_dalloc(arena_t* arena,void* ptr){
    intptr_t addr = (intptr_t)ptr;
    chunk_node_t snode;
    chunk_node_init(&snode,nullptr,0,addr,false);
    chunk_node_t* cnode = search_cnode(&arena->chunk_in_use,&snode.anode);
    rb_delete(&arena->chunk_in_use,&cnode->anode);
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

void clear_arena(arena_t* arena){
    while (atomic_load(&arena->threads)!=0);
    cleanup_map(&arena->chunk_dirty_szad);
    cleanup_map(&arena->chunk_cached_szad);
    cleanup_map(&arena->chunk_in_use);
    cleanup_map(&arena->chunk_nodes.chunk_in_use);
}

int addr_to_pid(intptr_t chunkaddr,intptr_t addr){
    m_at(is_alignof(chunkaddr-CHUNKHEADER,CHUNKSIZE),"chunkaddr is not aligned\n");
    intptr_t content = chunkaddr + sc_headersize - 1;
    return NEXT_ALIGN(addr-content,PAGE)>>PAGESHIFT;
}

void init_spanbin(spanbin_t* bin,std::size_t regsize){
    smutex_init(&bin->mtx,false);
    bin->cur = nullptr;
    bin->spannum = 0;
    bin->span_not_full = 0;
    // todo : tune this
    int span_minsize = 16*1024;
    int _regnum = span_minsize/regsize;
    int regnum = smax(REGMAX,_regnum);

    m_at(is_alignof(sc_maxsize,PAGE),"sc_headersize is not a multiple of page size\n");
    while (regnum*regsize>sc_maxsize)
        --regnum;
    bin->regnum = regnum;
    bin->regsize = regsize;
    bin->spansize = NEXT_ALIGN(regsize*regnum,PAGE);
    m_at(bin->spansize<=sc_maxsize,"spansize is too big\n");
    slog(LEVELA,"regnum:%d regsize:%lu spansize:%lu\n",regnum,regsize,bin->spansize);
}

void init_spanlists(span_list_t* slist,int sizeclass){
    smutex_init(&slist->mtx,false);
    slnode_init(&slist->spans);
    slist->avail = 0;
    int size = regsize_to_bin[sizeclass];
    int max_cached_size = 64*1024;
    int min_cached_num = 4;
    slist->max_avail = smax(max_cached_size/size,min_cached_num);
    slog(LEVELA,"spanlist[%d] - max_avail:%d\n",sizeclass,slist->max_avail);
}

void cal_sc_pagenum(std::size_t scsize){
    std::size_t a = CHUNKHEADER, b = scsize;
    sc_pagenum = 0;
    for (;;){
        a += sizeof(sbits)+sizeof(span_t);
        b -= PAGE;
        if (a>b)
            break;
        ++sc_pagenum;
    }
    sc_maxsize = sc_pagenum*PAGE;
    sc_headersize = scsize-sc_maxsize-CHUNKHEADER;
    slog(LEVELA,"sc_pagenum:%d sc_maxsize:%lu sc_headersize:%lu\n",sc_pagenum,sc_maxsize,sc_headersize);
}

int size_class(std::size_t size){
    if (size>regsize_to_bin[NBINS+NLBINS-1])
        return -1;
    else if (size<=8)
        return 0;
    else if (size<=16)
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
    ret += diff<<2;
    size >>= (diff+4);
    return ret + (size&3) + 4;
}

// just let chunkaddr to be the start address of sbits
span_t* pid_to_spanmeta(intptr_t chunkaddr,int pid){
    m_at(is_alignof(chunkaddr-CHUNKHEADER,CHUNKSIZE),"chunkaddr is wrong\n");
    return (span_t*)(chunkaddr + (sc_pagenum<<SBITSSHIFT) + (pid-1)*sizeof(span_t));
}

sbits* pid_to_sbits(intptr_t chunkaddr,int pid){
    m_at(is_alignof(chunkaddr-CHUNKHEADER,CHUNKSIZE),"chunkaddr is wrong\n");
    return (sbits*)(chunkaddr + ((pid-1)<<SBITSSHIFT));
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

void reg_chunk(chunk_node_t* cnode,bool huge){
    if (likely(!huge)){
        arena_t* arena_ = cnode->arena;
        rb_insert(&arena_->chunk_in_use,&cnode->anode);
    }else{
        smutex_lock(&hcs_mtx);
        rb_insert(&huge_chunks,&cnode->anode);
        smutex_unlock(&hcs_mtx);
    }
}

void unreg_chunk(chunk_node_t* cnode,bool huge){
    if (likely(!huge)){
        arena_t* arena_ = cnode->arena;
        rb_delete(&arena_->chunk_in_use,&cnode->anode);
    }else{
        smutex_lock(&hcs_mtx);
        rb_delete(&huge_chunks,&cnode->anode);
        smutex_unlock(&hcs_mtx);
    }
}

void before_arena_init(){
    rb_init(&huge_chunks,bigger_ad,equal_ad);
    smutex_init(&hcs_mtx,false);
    cal_sc_pagenum(SPANCSIZE);
}

void before_arena_destroy(){
    smutex_lock(&hcs_mtx);
    cleanup_map(&huge_chunks);
    smutex_unlock(&hcs_mtx);
}

void init_arena(arena_t* arena){
    arena->threads = 0;
    smutex_init(&arena->arena_mtx,true);
    rb_init(&arena->chunk_in_use,bigger_ad,equal_ad);

    nodes_init(&arena->chunk_nodes);
    
    arena->chunk_spared = nullptr;
    rb_init(&arena->chunk_cached_ad,bigger_ad,equal_ad);
    rb_init(&arena->chunk_cached_szad,bigger_szad,equal_szad);
    rb_init(&arena->chunk_dirty_ad,bigger_ad,equal_ad);
    rb_init(&arena->chunk_dirty_szad,bigger_szad,equal_szad);

    rb_init(&arena->spanavail,bigger_szad_span,equal_szad_span);
    rb_init(&arena->spandirty,bigger_szad_span,equal_szad_span);

    lnode_init(&arena->ldirty);
    lnode_init(&arena->lchunkdirty);

    arena->all_size = 0;
    arena->dirty_size = 0;

    for (int i=0;i<NBINS;++i){
        spanbin_t* bin = &arena->bins[i];
        init_spanbin(bin,regsize_to_bin[i]);
        rb_init(&bin->spans,bigger_ad_span,equal_ad_span);
    }

    for (int i=0;i<NBINS+NLBINS;++i)
        init_spanlists(&arena->spanlists[i],i);
}

chunk_node_t* search_chunk_from_map(arena_t* arena,std::size_t size,bool dirty){
    rb_tree_t<chunk_node_t>* tree;
    if (dirty)
        tree = &arena->chunk_dirty_szad;
    else
        tree = &arena->chunk_cached_szad;
    
    chunk_node_t kchunk;
    kchunk.chunk_size = size;
    kchunk.start_addr = -1;
    rbnode_init(&kchunk.anode,&kchunk,true);
    chunk_node_t* _chunk = fsearch_cnode(tree,&kchunk);
    if (_chunk==nullptr)
    	return nullptr;
    delete_map(arena,_chunk,dirty);
    return _chunk;
}

chunk_node_t* make_chunknode(arena_t* arena,intptr_t start_addr,bool huge){
    m_at(is_alignof(start_addr,CHUNKSIZE),"start_addr is not aligned\n");
    chunk_node_t* cnode = nullptr;
    if (likely(ISEMBED(huge)))
        cnode = (chunk_node_t*)start_addr;
    else 
        cnode = alloc_chunk_node(&arena->chunk_nodes);
    m_at(cnode!=nullptr,"chunk node should not be null\n");
    return cnode;
}

// 'chunk' is what i want
chunk_node_t* split_chunk(arena_t* arena,chunk_node_t* chunk,std::size_t size){
    chunk_node_t* left = nullptr;
    std::size_t left_size = chunk->chunk_size-size;
    if (left_size!=0 && is_multipleof(left_size,CHUNKSIZE)){
        intptr_t left_addr = chunk->start_addr+size;
        left = (chunk_node_t*)left_addr;
        chunk_node_init(left,arena,left_size,left_addr,false);
    }
    // have to handle 'chunk_size' in this way
    // because don't know if there is a constrain for 'size'
    chunk->chunk_size = size;
    return left;
}

// if not huge,size includes the size of chunk header
chunk_node_t* chunk_from_map(arena_t* arena,std::size_t size,bool dirty,bool huge){
    chunk_node_t* _chunk = search_chunk_from_map(arena,size,dirty);
    if (_chunk==nullptr)
        return nullptr;

    if (_chunk->chunk_size>size){
        chunk_node_t* left = split_chunk(arena,_chunk,size);
        if (left!=nullptr)
            insert_map(arena,left,dirty);
    }

    chunk_node_t* cnode = make_chunknode(arena,_chunk->start_addr,huge);
    chunk_node_init(cnode,arena,_chunk->chunk_size,_chunk->start_addr,ISEMBED(huge));
    reg_chunk(cnode,huge);
    return cnode;
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
            }
            if (prev!=nullptr && mergable(prev,chunk)){
                delete_map(arena,prev,dirty);
                prev->chunk_size += chunk->chunk_size;
                chunk = prev;
            }
            insert_map(arena,chunk,dirty);
            return;
        }
        prev = now;
    }
}

void* new_chunk(arena_t* arena,std::size_t size,bool huge){
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
        unlink_chunkdirty_arena(arena,arena->chunk_spared);
        if (chunk->chunk_size==size){
            arena->chunk_spared = nullptr;
            chunk_node_t* cnode = make_chunknode(arena,chunk->start_addr,huge);
            chunk_node_init(cnode,arena,chunk->chunk_size,chunk->start_addr,ISEMBED(huge));
            reg_chunk(cnode,huge);
            m_at(chunk_is_alignof(cnode->start_addr,CHUNKSIZE,huge),
                            "start_addr (%lx) is not aligned\n",cnode->start_addr);
            return (void*)cnode->start_addr;
        }
        arena->chunk_spared = split_chunk(arena,chunk,size);
        if (arena->chunk_spared!=nullptr)
            link_chunkdirty_arena(arena,arena->chunk_spared);
        else 
            arena->dirty_size -= chunk->chunk_size - size;

        chunk_node_t* cnode = make_chunknode(arena,chunk->start_addr,huge);
        chunk_node_init(cnode,arena,chunk->chunk_size,chunk->start_addr,ISEMBED(huge));
        reg_chunk(cnode,huge);
        m_at(chunk_is_alignof(cnode->start_addr,CHUNKSIZE,huge),
                        "start_addr (%lx) is not aligned\n",cnode->start_addr);
        return (void*)cnode->start_addr;
    }

    chunk = chunk_from_map(arena,size,true,huge);
    if (chunk!=nullptr) {
        m_at(chunk_is_alignof(chunk->start_addr,CHUNKSIZE,huge),
                        "start_addr (%lx) is not aligned\n",chunk->start_addr);
        return (void*)chunk->start_addr;
    }
    
    chunk = chunk_from_map(arena,size,false,huge);
    if (chunk!=nullptr) {
        m_at(chunk_is_alignof(chunk->start_addr,CHUNKSIZE,huge),
                        "start_addr (%lx) is not aligned\n",chunk->start_addr);
        return (void*)chunk->start_addr;
    }

    chunk_node_t* cnode = base_alloc(arena,CHUNKSIZE,size,false,huge);
    m_at(cnode!=nullptr,"fail to allocate memory from os\n");
    m_at(chunk_is_alignof(cnode->start_addr,CHUNKSIZE,huge),
                    "start_addr (%lx) is not aligned\n",cnode->start_addr);
    reg_chunk(cnode,huge);
    return (void*)cnode->start_addr;
}

void delete_chunk(arena_t* arena,void* addr,chunk_node_t* chunk,bool dirty,bool huge){
    if (likely(chunk==nullptr)){
        m_at(addr!=nullptr,"addr should not be null\n");
        chunk_node_t snode;
        snode.start_addr = (intptr_t)addr;
        rbnode_init(&snode.anode,&snode,true);
        rb_tree_t<chunk_node_t>* target;
        if (likely(!huge)){
            chunk = search_cnode(&arena->chunk_in_use,&snode.anode);
        }else{
            smutex_lock(&hcs_mtx);
            chunk = search_cnode(&huge_chunks,&snode.anode);
            smutex_unlock(&hcs_mtx);
        }
    }
    m_at(chunk!=nullptr,"chunk should not be null\n");
    unreg_chunk(chunk,huge);

    if (huge) {
        chunk_node_t* chunk_ = chunk;
        m_at(is_alignof(chunk->start_addr,CHUNKSIZE),"start addr is not aligned\n");
        chunk = (chunk_node_t*)chunk->start_addr;
        chunk_node_init(chunk,arena,chunk_->chunk_size,chunk_->start_addr,false);
        dalloc_node(&arena->chunk_nodes,chunk_);
    } else {
        TORAWCHUNK(chunk);
    }

    m_at(is_alignof(chunk->start_addr,CHUNKSIZE),"chunk_addr is not aligned\n");
    m_at(is_multipleof(chunk->chunk_size,CHUNKSIZE),"chunk_size is not a multiple of CHUNKSIZE\n");

    if (!dirty){
        chunk_to_map(arena,chunk,dirty);
    } else if (arena->chunk_spared==nullptr) {
        arena->chunk_spared = chunk;
        arena->dirty_size += chunk->chunk_size;
        // ALWAYS have to take special care of chunk_spared
        link_chunkdirty_arena(arena,chunk);
    } else if (behind_addr(chunk,arena->chunk_spared) && mergable(chunk,arena->chunk_spared)) {    
        unlink_chunkdirty_arena(arena,arena->chunk_spared);
        chunk_node_t* chunk_spared_ = arena->chunk_spared;
        // have to switch 'chunk_node' because chunk_node is embedded
        arena->chunk_spared = chunk;
        arena->chunk_spared->chunk_size += chunk_spared_->chunk_size;
        link_chunkdirty_arena(arena,arena->chunk_spared);
        arena->dirty_size += chunk->chunk_size;
    } else if (behind_addr(arena->chunk_spared,chunk) && mergable(arena->chunk_spared,chunk)) {
        arena->chunk_spared->chunk_size += chunk->chunk_size;
        arena->dirty_size += chunk->chunk_size;
    } else if (behind_addr(chunk,arena->chunk_spared)) {
        chunk_node_t* _chunk_spared = arena->chunk_spared;
        arena->dirty_size -= _chunk_spared->chunk_size;
        unlink_chunkdirty_arena(arena,_chunk_spared);
        arena->chunk_spared = chunk;
        link_chunkdirty_arena(arena,chunk);
        arena->dirty_size += chunk->chunk_size;
        chunk_to_map(arena,_chunk_spared,dirty);
    } else {
        chunk_to_map(arena,chunk,dirty);
    }
    return;
}

// have to make sure 'chunk_size' is a power of 2
// since we talk about ptr which is in chunk here,
// so this chunk has to be used for small or large allocations
// it should be safe to assume there is a CHUNKHEADER
bool ptr_in_chunk(intptr_t chunk_addr,intptr_t addr){
    addr &= ~(SPANCSIZE-1);
    return chunk_addr-CHUNKHEADER == addr;
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
    intptr_t chunkaddr = addr_to_chunk_start(start_pos);
    chunkaddr = jump_to_sbit(chunkaddr);
    int pid_head = addr_to_pid(chunkaddr,start_pos);
    int pid_tail = addr_to_pid(chunkaddr,start_pos+size-1);
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
    intptr_t chunkaddr = addr_to_chunk_start(start_pos);
    chunkaddr = jump_to_sbit(chunkaddr);
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

// chunkaddr follows chunkheader (assume there is one)
span_t* chunk_to_bigspan(void* chunkaddr,arena_t* arena){
    m_at(is_alignof(((intptr_t)chunkaddr)-CHUNKHEADER,CHUNKSIZE),"chunkaddr is not aligned\n");
    memset(chunkaddr,0,sc_headersize);
    intptr_t caddr = (intptr_t)chunkaddr;
    span_t* span_head = pid_to_spanmeta(jump_to_sbit(caddr-CHUNKHEADER),1);
    span_head->spansize = sc_maxsize;
    span_head->start_pos = caddr+sc_headersize;
    rbnode_init(&span_head->anode,span_head,true);
    sbits_large(span_head->start_pos,sc_maxsize,N,false,-1);
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

    intptr_t chunkaddr = addr_to_chunk_start(_spanaddr);
    chunkaddr = jump_to_sbit(chunkaddr);
    span_t* _span = pid_to_spanmeta(chunkaddr,addr_to_pid(chunkaddr,_spanaddr));
    _span->spansize = _spansize;
    _span->start_pos = _spanaddr;
    return _span;
}

span_t* fsearch_span(rb_tree_t<span_t>* tree,span_t* key){
    rbnode_t<span_t>* node = rb_fsearch(tree,&key->anode);
    if (node==nullptr)
        return nullptr;
    return (span_t*)node->ptr;
}

span_t* span_from_map(arena_t* arena,std::size_t size,bool dirty){
    rb_tree_t<span_t>* tree;
    if (dirty)
        tree = &arena->spandirty;
    else
        tree = &arena->spanavail;
    
    span_t kspan;
    kspan.spansize = size;
    kspan.start_pos = -1;
    rbnode_init(&kspan.anode,&kspan,true);
    span_t* _span = fsearch_span(tree,&kspan);
    if (_span==nullptr)
        return nullptr; 
    delete_map_span(arena,_span,dirty);
    if (_span->spansize==size)
        return _span;
    span_t* left = split_bigspan(_span,size);
    sbits_large(left->start_pos,left->spansize,N,dirty,-1);
    rbnode_init(&left->anode,left,true);
    insert_map_span(arena,left,dirty);
    return _span;
}

span_t* new_span(arena_t* arena,std::size_t size){
    span_t* span = span_from_map(arena,size,true);
    if (span!=nullptr)
        return span;

    span = span_from_map(arena,size,false);
    if (span!=nullptr)
        return span;
    
    void* ptr = new_chunk(arena,SPANCSIZE,false);
    intptr_t ptr_ = (intptr_t)ptr;
    m_at(ptr!=nullptr,"fail to allocate memory from os\n");
    m_at(is_alignof(ptr_-CHUNKHEADER,CHUNKSIZE),"ptr (%lx) is not aligned\n",(intptr_t)ptr);
    span = chunk_to_bigspan(ptr,arena);
    span_t* left = split_bigspan(span,size);
    sbits_large(span->start_pos,span->spansize,N,false,-1);
    if (left==nullptr)
        return span;
    sbits_large(left->start_pos,left->spansize,N,false,-1);
    rbnode_init(&left->anode,left,true);
    insert_map_span(arena,left,false);
    return span;
}

span_t* try_unlink_spanlist(arena_t* arena,int binid){
    span_list_t* spanlist = &arena->spanlists[binid];
    smutex_lock(&spanlist->mtx);
    if (spanlist->avail==0){
        smutex_unlock(&spanlist->mtx);
        return nullptr;
    }
    span_t* span = node_to_struct(span_t,lspans,spanlist->spans.next);
    spanlist->spans.next = span->lspans.next;
    span->lspans.next = nullptr;
    --spanlist->avail;
    smutex_unlock(&spanlist->mtx);
    return span;
}

span_t* new_span_for_bin(arena_t* arena,int binid){
    spanbin_t* bin = &arena->bins[binid];
    std::size_t rsize = bin->spansize;
    
    span_t* span = try_unlink_spanlist(arena,binid);
    if (span!=nullptr)
        return span;

    smutex_lock(&arena->arena_mtx);
    span = new_span(arena,rsize);
    if (span==nullptr){
        smutex_unlock(&arena->arena_mtx);
        return nullptr;
    }
    sbits_small_alloc(span->start_pos,rsize,binid);
    smutex_unlock(&arena->arena_mtx);
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
        slnode_t* ret = span->lfree;
        span->lfree = ret->next;
        return (void*)ret;
    }
    if (span_is_on_list(span))
        return nullptr;
    intptr_t ret = span->next_free;
    span->next_free += span->regsize;
    return (void*)ret;
}

void* find_span_and_alloc(arena_t* arena,int binid,span_t** from){
    spanbin_t* bin = &arena->bins[binid];
    if (bin->spannum==0){
        if (new_span_for_bin(arena,binid)==nullptr){
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
        if (span==nullptr)
            return nullptr;
    }
    void* ret = alloc_reg(span);
    if (span_is_full(span))
        --bin->span_not_full;
    else if (from!=nullptr)
        *from = span;
    return ret;
}

void* alloc_small(arena_t* arena,std::size_t size){
    int binid = size_class(size);
    if (binid>=NBINS)
        return nullptr;
    spanbin_t* bin = &arena->bins[binid];
    smutex_lock(&bin->mtx);
    void* ret = find_span_and_alloc(arena,binid,nullptr);
    smutex_unlock(&bin->mtx);
    return ret;
}

void alloc_small_batch(arena_t* arena,int binid,void** ptrs,int want){
    if (binid>=NBINS)
        return;
    spanbin_t* bin = &arena->bins[binid];
    smutex_lock(&bin->mtx);
    span_t* from = nullptr;
    for (int i=0;i<want;++i){
        void* _ptr;
        if (from!=nullptr){
            _ptr = alloc_reg(from);
            if (_ptr!=nullptr){
                ptrs[i] = _ptr;
                continue;
            }
            from = nullptr;
        }
        _ptr = find_span_and_alloc(arena,binid,&from);
        m_at(_ptr!=nullptr,"fail to get memory from os\n");
        ptrs[i] = _ptr;
    }
    smutex_unlock(&bin->mtx);
}

bool mergeable_span(span_t* a,span_t* b){  
    return a->start_pos+a->spansize == b->start_pos;
}

bool behind_addr_span(span_t* a,span_t* b){
    return a->start_pos < b->start_pos;
}

bool try_delete_span_chunk(arena_t* arena,span_t* span,bool dirty){
    if (span->spansize==sc_maxsize){
        delete_chunk(arena,(void*)(addr_to_chunk_start(span->start_pos)+CHUNKHEADER),nullptr,dirty,false);
        return true;
    }
    return false;
}

void span_is_free(arena_t* arena,span_t* span,bool dirty){
    if (unlikely(try_delete_span_chunk(arena,span,dirty)))
        return;
    int d = (dirty) ? Y : N;
    intptr_t saddr = span->start_pos;
    intptr_t chunkaddr = addr_to_chunk_start(saddr);
    chunkaddr = jump_to_sbit(chunkaddr);
    int pid = addr_to_pid(chunkaddr,saddr);
    if (pid>1){
        int prev_pid = pid-1;
        sbits* sb = pid_to_sbits(chunkaddr,prev_pid);
        if (ALLOC(sb)==N && DIRTY(sb)==d){ 
            int pageid = PAGEID(sb);
            prev_pid -= (pageid-1);
            span_t* prev = pid_to_spanmeta(chunkaddr,prev_pid);
            delete_map_span(arena,prev,dirty);
            prev->spansize += span->spansize;
            span = prev;
            pid = prev_pid;
        }
    }
    if (pid<sc_pagenum){
        int span_pages = NEXT_ALIGN(span->spansize,PAGE)>>PAGESHIFT;
        int next_pid = pid+span_pages;
        if (next_pid<=sc_pagenum){
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
            lnode_t* cnow_next = cnow->next;
            chunk_node_t* chunk = node_to_struct(chunk_node_t,ldirty,now);
            if (chunk!=arena->chunk_spared){
                // must unlink this in this way
                // or else the deleted chunk(prev) will be linked again
                rb_delete(&arena->chunk_dirty_szad,&chunk->anode);
                rb_delete(&arena->chunk_dirty_ad,&chunk->bnode);
                arena->dirty_size -= chunk->chunk_size;
            }else
                arena->chunk_spared = nullptr;
            fsize += chunk->chunk_size;
            page_to_corner((void*)chunk->start_addr,chunk->chunk_size);
            chunk_to_map(arena,chunk,false);
            cnow = cnow_next;
        }else{
            span_t* span = node_to_struct(span_t,ldirty,now);
            rb_delete(&arena->spandirty,&span->anode);
            arena->dirty_size -= span->spansize;
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
    // DO NOT forget this is a doubly linked list
    if (cnow!=&arena->lchunkdirty){
        arena->lchunkdirty.next = cnow;
        cnow->prev = &arena->lchunkdirty;
    }else
        lnode_init(&arena->lchunkdirty);
    arena->ldirty.next = now;
    now->prev = &arena->ldirty;
}

bool try_link_spanlist(arena_t* arena,span_t* span){
    int sc = size_class(span->spansize);
    span_list_t* spanlist = &arena->spanlists[sc];
    smutex_lock(&spanlist->mtx);
    if (spanlist->avail==spanlist->max_avail){
        smutex_unlock(&spanlist->mtx);
        return false;
    }
    span->nfree = 0;
    span->lfree = nullptr;
    span->next_free = span->start_pos; 
    slnode_init(&span->lspans);
    span->lspans.next = spanlist->spans.next;
    spanlist->spans.next = &span->lspans;
    ++spanlist->avail;
    smutex_unlock(&spanlist->mtx);
    return true;
}

void dalloc_small(arena_t* arena,void* ptr){
    intptr_t chunkaddr = addr_to_chunk_start((intptr_t)ptr);
    chunkaddr = jump_to_sbit(chunkaddr);
    int pid = addr_to_pid(chunkaddr,(intptr_t)ptr);
    sbits* bs = pid_to_sbits(chunkaddr,pid);
    int binid = BINID(bs);
    int pageid = PAGEID(bs);
    pid -= (pageid-1);
    spanbin_t* bin = &arena->bins[binid];
    span_t* span = pid_to_spanmeta(chunkaddr,pid);

    smutex_lock(&bin->mtx);
    bool was_full = span_is_full(span);
    slnode_t* free_prev = span->lfree;
    span->lfree = (slnode_t*)ptr;
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
    else
    	rb_delete(&bin->spans,&span->anode);
    smutex_unlock(&bin->mtx);

    if (try_link_spanlist(arena,span))
        return;

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
    
    span_t* span = try_unlink_spanlist(arena,binid);
    if (span!=nullptr)
        return (void*)span->start_pos;
    
    smutex_lock(&arena->arena_mtx);
    span = new_span(arena,size);
    m_at(span!=nullptr,"fail to get memory from os\n");
    sbits_large(span->start_pos,span->spansize,Y,true,binid);
    smutex_unlock(&arena->arena_mtx);
    lnode_init(&span->ldirty);
    return (void*)span->start_pos;
}

void alloc_large_batch(arena_t* arena,int binid,void** ptrs,int want){
    std::size_t size = regsize_to_bin[binid];
    int _get = 0;
    for (;_get<want;++_get){
        span_t* span = try_unlink_spanlist(arena,binid);
        if (span==nullptr)
            break;
        ptrs[_get] = (void*)span->start_pos;
    }
    if (_get == want)
        return;
    smutex_lock(&arena->arena_mtx);
    for (;_get<want;++_get){
        span_t* span = new_span(arena,size);
        m_at(span!=nullptr,"fail to get memory from os\n");
        sbits_large(span->start_pos,span->spansize,Y,true,binid);
        ptrs[_get] = (void*)span->start_pos;
    }
    smutex_unlock(&arena->arena_mtx);
}

void dalloc_large(arena_t* arena,void* ptr){
    intptr_t chunkaddr = addr_to_chunk_start((intptr_t)ptr);
    chunkaddr = jump_to_sbit(chunkaddr);
    int pid = addr_to_pid(chunkaddr,(intptr_t)ptr);
    span_t* span = pid_to_spanmeta(chunkaddr,pid);
    if (try_link_spanlist(arena,span))
        return;

    smutex_lock(&arena->arena_mtx);
    span_is_free(arena,span,true);
    std::size_t wanted = should_purge(arena);
    if (unlikely(wanted>0))
        purge(arena,wanted);
    smutex_unlock(&arena->arena_mtx);
}

void* alloc_huge(arena_t* arena,std::size_t size){
    smutex_lock(&arena->arena_mtx);
    void* chunk = new_chunk(arena,size,true);
    smutex_unlock(&arena->arena_mtx);
    m_at(chunk!=nullptr,"fail to get memory from os\n");
    m_at(is_alignof((intptr_t)chunk,CHUNKSIZE),"ptr is not aligned\n");
    return chunk;
}

void dalloc_huge(arena_t* arena,void* ptr){
    m_at(is_alignof((intptr_t)ptr,CHUNKSIZE),"ptr is not aligned\n");
    smutex_lock(&arena->arena_mtx);
    delete_chunk(arena,ptr,nullptr,true,true);
    std::size_t wanted = should_purge(arena);
    if (unlikely(wanted>0))
        purge(arena,wanted);
    smutex_unlock(&arena->arena_mtx);
}

void search_and_dalloc_huge(void* ptr){
    m_at(is_alignof((intptr_t)ptr,CHUNKSIZE),"ptr is not aligned\n");
    chunk_node_t snode;
    snode.start_addr = (intptr_t)ptr;
    rbnode_init(&snode.anode,&snode,true);
    smutex_lock(&hcs_mtx);
    chunk_node_t* chunk = search_cnode(&huge_chunks,&snode.anode);
    smutex_unlock(&hcs_mtx);

    m_at(chunk!=nullptr,"chunk should not be null\n");
    arena_t* arena = chunk->arena;
    smutex_lock(&arena->arena_mtx);
    delete_chunk(chunk->arena,nullptr,chunk,true,true);
    std::size_t wanted = should_purge(arena);
    if (unlikely(wanted>0))
        purge(arena,wanted);
    smutex_unlock(&arena->arena_mtx);
}
