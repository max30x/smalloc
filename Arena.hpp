#pragma once

#include <string.h>
#include <cstdint>
#include <stdatomic.h>

#include "Os.hpp"
#include "RbTree.hpp"
#include "Common.hpp"
#include "Mutex.hpp"

#define CHUNKSIZE 2*1024*1024

struct arena;

// when we take a chunk from arena,
// header of this chunk should be embedded into the start of this chunk.
// but attributes like 'start_addr' and 'chunk_size' are about the whole chunk,
// which means CHUNKHEADER size is not taken into consideration
struct chunk_node{
    struct arena* arena;

    intptr_t start_addr;

    std::size_t chunk_size;

    lnode_t ldirty;

    lnode_t lchunk;

    // when in use or in szad
    rbnode_t<struct chunk_node> anode;
    
    rbnode_t<struct chunk_node> bnode;
};

using chunk_node_t = struct chunk_node;

#define CHUNKHEADER sizeof(chunk_node_t)

#define ISEMBED(huge) (!huge)

#define TORAWCHUNK(chunk)               \
do{                                     \
    chunk->chunk_size += CHUNKHEADER;   \
    chunk->start_addr -= CHUNKHEADER;   \
}while (0)              

template<typename T>
struct mnode{
    smutex_t mtx;
    T* node_free;
    chunk_node_t* node_chunk;
    std::size_t nsize;
    long offset;
    rb_tree_t<chunk_node_t> chunk_in_use;
};

template<typename T>
using mnode_t = struct mnode<T>;

#define SPANCSIZE CHUNKSIZE
#define REGMIN 128
#define REGMAX 512

#define is_alignof(a,b) \
    (((a) != 0) && ((a) % (b) == 0))

#define is_multipleof(a,b) \
    (((a) != 0) && ((a) % (b) == 0))

// addr here should have already been moved forward because of chunkheader 
static bool chunk_is_alignof(intptr_t addr,std::size_t align,bool huge){
    if (!huge)
        addr -= CHUNKHEADER;
    bool ret = is_alignof(addr,align);
    return ret;
}

struct span{
    intptr_t start_pos;

    std::size_t regsize;

    std::size_t spansize;

    int regnum;

    // block num in freelist
    int nfree;

    // use this to chain free regions together
    slnode_t* lfree;
    
    union{
        // link with other dirty spans
        lnode_t ldirty;

        // link with other spans in spanlist
        slnode_t lspans;
    };

    intptr_t next_free;
    
    rbnode_t<struct span> anode;
};

using span_t = struct span;

#define addr_to_chunk_start(ptr) \
    (ptr & (~(CHUNKSIZE-1)))

// addr is the start address of a chunk
// not that 'start address' in 'chunk_node_t'
#define jump_to_sbit(addr) \
    (addr+CHUNKHEADER)

#define SBITSSHIFT 3
#define sbits unsigned long

#define get_sbits(addr,pid) \
    (sbits*)(addr+sizeof(sbits)*(pid-1))

// for x86
#define ff1_long(num) ({    \
unsigned long ret;          \
__asm__ ("bsr %1,%0\n\t"    \
        :"+r"(ret)          \
        :"r"(num));         \
ret;                        \
})

#define SETPAGEID(bits,pid) do{ \
    *(bits) &= ~((1<<10)-1);    \
    *(bits) |= pid;             \
}while(0)

#define SETBINID(bits,bid) do{      \
    *(bits) &= ~(((1<<8)-1)<<10);   \
    *(bits) |= bid<<10;             \
}while(0)

#define SETALLOC(bits,state) do{    \
    *(bits) &= ~(1<<18);            \
    *(bits) |= state<<18;           \
}while(0)

#define SETTYPE(bits,type) do{      \
    *(bits) &= ~(((1<<2)-1)<<19);   \
    *(bits) |= type<<19;            \
}while(0)

#define SETDIRTY(bits,state) do{    \
    *(bits) &= ~(1<<21);            \
    *(bits) |= state<<21;           \
}while (0)
    

#define SMALL 0
#define LARGE 1
#define HUGE 2

#define Y 1
#define N 0 // this has to be 0

// | ... | dirty(1bit) | type(2bit) | alloc(1bit) | binid(8bit) | pageid (10bit) |

#define PAGEID(bits) (*(bits) & ((((sbits)1)<<10)-1))
#define BINID(bits) ((*(bits)>>10) & ((((sbits)1)<<8)-1))
#define ALLOC(bits) (*(bits)>>18 & 1) 
#define TYPE(bits) ((*(bits)>>19) & ((1<<2)-1))
#define DIRTY(bits) (*(bits)>>21 & 1)

struct spanbin{
    smutex_t mtx;
    int spannum;
    int span_not_full;

    int regnum;
    std::size_t regsize;
    std::size_t spansize;

    span_t* cur;
    rb_tree_t<span_t> spans;
};

using spanbin_t = struct spanbin;


#define NBINS 36
#define NLBINS 28
static int regsize_to_bin[NBINS+NLBINS] = {
    8,
    16, 32, 48, 64, 80, 96, 112, 128,
    160, 192, 224, 256,
    320, 384, 448, 512,
    640, 768, 896, 1024,
    1280, 1536, 1792, 2048,
    2560, 3072, 3584, 4096,
    5120, 6144, 7168, 8192,
    10240, 12288, 14336,
    16*1024,
    20*1024,24*1024,28*1024,32*1024,
    40*1024,48*1024,56*1024,64*1024,
    80*1024,96*1024,112*1024,128*1024,
    160*1024,192*1024,224*1024,256*1024,
    320*1024,384*1024,448*1024,512*1024,
    640*1024,768*1024,896*1024,1024*1024,
    1280*1024,1536*1024,1792*1024
};

struct span_list{
    int max_avail;
    int avail;
    slnode_t spans;
    smutex_t mtx;
};
using span_list_t = struct span_list;

#define DIRTYMAX(all) (all) >> 3

struct arena{
    // todo:this lock is annoying
    smutex_t arena_mtx;

    atomic_int threads;

    mnode_t<chunk_node_t> chunk_nodes; 

    rb_tree_t<chunk_node_t> chunk_in_use;

    chunk_node_t* chunk_spared;

    rb_tree_t<chunk_node_t> chunk_dirty_szad;
    rb_tree_t<chunk_node_t> chunk_dirty_ad;

    rb_tree_t<chunk_node_t> chunk_cached_szad;
    rb_tree_t<chunk_node_t> chunk_cached_ad;

    rb_tree_t<span_t> spanavail;
    rb_tree_t<span_t> spandirty;

    lnode_t ldirty;
    lnode_t lchunkdirty;

    std::size_t dirty_size;
    std::size_t all_size;

    spanbin_t bins[NBINS];

    span_list_t spanlists[NBINS+NLBINS];
};

using arena_t = struct arena;

int size_class(std::size_t size);
int addr_to_pid(intptr_t chunkaddr,intptr_t addr);
sbits* pid_to_sbits(intptr_t chunkaddr,int pid);

bool ptr_in_chunk(intptr_t chunk_addr,intptr_t addr);

void before_arena_init();
void before_arena_destroy();

void init_arena(arena_t* arena);
void clear_arena(arena_t* arena);
void* alloc_small(arena_t* arena,std::size_t size);
void alloc_small_batch(arena_t* arena,int binid,void** ptrs,int want);
void dalloc_small(arena_t* arena,void* ptr);
void* alloc_large(arena_t* arena,std::size_t size);
void alloc_large_batch(arena_t* arena,int binid,void** ptrs,int want);
void dalloc_large(arena_t* arena,void* ptr);
void* alloc_huge(arena_t* arena,std::size_t size);
void dalloc_huge(arena_t* arena,void* ptr);
void search_and_dalloc_huge(void* ptr);