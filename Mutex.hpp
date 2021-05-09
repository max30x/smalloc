#pragma once

#include <pthread.h>

#include "Common.hpp"

struct smutex{
    pthread_mutex_t mtx;
};

using smutex_t = struct smutex;

FORCE_INLINE static void smutex_init(smutex_t* smtx){
    pthread_mutex_init(&smtx->mtx,nullptr);
}

FORCE_INLINE static void smutex_lock(smutex_t* smtx){
    pthread_mutex_lock(&smtx->mtx);
}

FORCE_INLINE static void smutex_unlock(smutex_t* smtx){
    pthread_mutex_unlock(&smtx->mtx);
}