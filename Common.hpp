#pragma once

#include <stdio.h>
#include <pthread.h>

//#define SINFO

#define NEXT_ALIGN(num,align) \
    ((num+align-1) & (~(align-1)))

#define likely(x) __builtin_expect(!!(x), 1)       
#define unlikely(x) __builtin_expect(!!(x), 0) 

#define FORCE_INLINE __attribute__((always_inline))

#define smin(a,b) ({            \
    typeof((a)) ret;            \
    ret = ((a)<(b))?(a):(b);    \
    ret;                        \
})

#define smax(a,b) ({            \
    typeof((a)) ret;            \
    ret = ((a)>(b))?(a):(b);    \
    ret;                        \
})

struct slnode{
    struct slnode* next;
};
using slnode_t = struct slnode;

inline void slnode_init(slnode_t* n){
    n->next = nullptr;
}

struct lnode{
    struct lnode* next;
    struct lnode* prev;
};
using lnode_t = struct lnode;

#define node_to_struct(type,member,ptr) \
    (type*)((char*)ptr-(unsigned long)&((type*)0)->member)

static inline void lnode_init(lnode_t* n){
    n->next = n;
    n->prev = n;
}

// link b with a
static inline void link_lnode(lnode_t* a,lnode_t* b){
    b->next = a->next;
    a->next->prev = b;
    a->next = b;
    b->prev = a;
}

// when you unline b from list then b point to itself
static inline void unlink_lnode(lnode_t* b){
    lnode_t* prev = b->prev;
    if (prev==b)
        return;
    prev->next = b->next;
    b->next->prev = prev;

    b->next = b;
    b->prev = b;
}

#define s_assert(x,s,...)               \
do{                                     \
    if (!(x)){                          \
        printf("line:%d ",__LINE__);    \
        printf(s,##__VA_ARGS__);        \
        abort();                        \
    }                                   \
}while(0)


#define LEVELA 1
#define LEVELB 2
#define LEVELC 3

#define IOSTREAM stdout
#define SINFO_LEVEL LEVELB
#ifdef SINFO
    #define slog(level,s,...)                                               \
    do{                                                                     \
    if (SINFO_LEVEL>=level){                                                \
        fprintf(IOSTREAM,"line:%d thread:%ld ",__LINE__,pthread_self());    \
        fprintf(IOSTREAM,s,##__VA_ARGS__);                                  \
    }                                                                       \
    }while(0)
#else
    #define slog(s,...)
#endif
    