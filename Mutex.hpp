#pragma once

#include <pthread.h>
#include <stdio.h>

#include "Common.hpp"

struct smutex{
    pthread_mutex_t mtx;
    pthread_mutexattr_t attr;
};

using smutex_t = struct smutex;

FORCE_INLINE static void smutex_init(smutex_t* smtx,bool recursive){
    pthread_mutexattr_init(&smtx->attr);
    int kind = PTHREAD_MUTEX_NORMAL;
    if (recursive) {
        kind = PTHREAD_MUTEX_RECURSIVE;
    }
    pthread_mutexattr_settype(&smtx->attr,kind);
    pthread_mutex_init(&smtx->mtx,&smtx->attr);
}

FORCE_INLINE static void smutex_lock(smutex_t* smtx){
    pthread_mutex_lock(&smtx->mtx);
}

FORCE_INLINE static void smutex_unlock(smutex_t* smtx){
    pthread_mutex_unlock(&smtx->mtx);
}