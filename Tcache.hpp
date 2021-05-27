#pragma once

#include <stdatomic.h>
#include <stdlib.h>

#include "Arena.hpp"
#include "Os.hpp"

#define CACHESIZE 64*1024
#define NCACHEDMIN 16
#define INITRATIO 3

struct tbin{
    int ratio;
    int avail;
    int ncached;
    std::size_t size;
    void** ptrs;
};

using tbin_t = struct tbin;

struct tcache{
    slnode_t tnode;
    arena_t* arena;
    tbin_t bins[NBINS+NLBINS];
};

using tcache_t = struct tcache;

struct tcaches{
    smutex_t mtx;
    slnode_t tlink;

    int tcsize;
    char* chunk;
    int offset;
    int limit;
    slnode_t mlink;
};

using tcaches_t = struct tcaches;