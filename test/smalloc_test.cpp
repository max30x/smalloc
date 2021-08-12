#include <stdio.h>
#include <pthread.h>
#include <thread>
#include <stdlib.h>

#include "Arena.hpp"
#include "Smalloc.hpp"
#include "ScopeTimer.hpp"

#define NUM1 10000
void alloc_small_test(arena_t* arena,std::size_t size){
    void* ptrs[NUM1];
    for (int i=0;i<NUM1;++i)
        ptrs[i] = alloc_small(arena,size);
    for (int i=0;i<NUM1;++i)
        dalloc_small(arena,ptrs[i]);
    for (int i=0;i<NUM1;++i)
        ptrs[i] = alloc_small(arena,size);
    for (int i=0;i<NUM1;++i)
        dalloc_small(arena,ptrs[i]);
    printf("alloc_small_test --- pass\n");
}

#define NUM2 100
void alloc_large_test(arena_t* arena,std::size_t size){
    void* ptrs[NUM2];
    for (int i=0;i<NUM2;++i)
        ptrs[i] = alloc_large(arena,size);
    for (int i=0;i<NUM2;++i)
        dalloc_large(arena,ptrs[i]);
    for (int i=0;i<NUM2;++i)
        ptrs[i] = alloc_large(arena,size);
    for (int i=0;i<NUM2;++i)
        dalloc_large(arena,ptrs[i]);
    printf("alloc_large_test --- pass\n");
}

#define NUM3 5000
#define NUM4 50
void alloc_small_large_test(arena_t* arena,std::size_t small_size,std::size_t large_size){
    void* small_ptrs[NUM3];
    void* large_ptrs[NUM4];
    for (int i=0;i<NUM3;++i)
        small_ptrs[i] = alloc_small(arena,small_size);
    for (int i=0;i<NUM4;++i)
        large_ptrs[i] = alloc_large(arena,large_size);
    for (int i=0;i<NUM4;++i)
        dalloc_large(arena,large_ptrs[i]);
    void* small_ptrs_2[NUM3];
    for (int i=0;i<NUM3;++i)
        small_ptrs_2[i] = alloc_small(arena,small_size);
    for (int i=0;i<NUM3;++i){
        dalloc_small(arena,small_ptrs[i]);
        dalloc_small(arena,small_ptrs_2[i]);
    }
    printf("alloc_small_large_test --- pass\n");
}

void alloc_large_test2(arena_t* arena){
    void* ptrs[104];
    for (int i=0;i<104;++i)
        ptrs[i] = alloc_large(arena,17*1024);
    for (int i=0;i<104;++i)
        dalloc_large(arena,ptrs[i]);
}

void smalloc_test_speed(std::size_t size,int num){
    void* ptrs[num];

    void* _p = smalloc(4);
    sfree(_p);

    {
        utils::ScopeTimer t;
        for (int i=0;i<num;++i)
            ptrs[i] = smalloc(size);
        for (int i=0;i<num;++i)
            sfree(ptrs[i]);
    }
    
    {
        utils::ScopeTimer t;
        for (int i=0;i<num;++i)
            ptrs[i] = malloc(size);
        for (int i=0;i<num;++i)
            free(ptrs[i]);
    }
    
}

void* smalloc_test_speed_noarg(void* arg){
    smalloc_test_speed(8,10000);
    return nullptr;
}

void smalloc_test(std::size_t size,int num){
    void* ptrs[num];

    for (int i=0;i<num;++i){
        ptrs[i] = smalloc(size);
    }

    for (int i=0;i<num;++i){
        sfree(ptrs[i]);
    }
}

void* smalloc_test_mt_rt(void* arg){
    smalloc_test(3*1024*1024,1);
    smalloc_test(17*1024,10);
    smalloc_test(4,1);
    smalloc_test(3*1024*1024,1);
    smalloc_test(33*1024,50);
    smalloc_test(37,10000);
    smalloc_test(4,10000);

    printf("tid:%ld --- smalloc_test_mt_rt finished\n",pthread_self());
    return nullptr;
}

void smalloc_test_multithread(){
    pthread_t t1,t2,t3,t4,t5,t6;
    pthread_create(&t1,nullptr,smalloc_test_mt_rt,nullptr);
    pthread_create(&t2,nullptr,smalloc_test_mt_rt,nullptr);
    pthread_create(&t3,nullptr,smalloc_test_mt_rt,nullptr);
    pthread_create(&t4,nullptr,smalloc_test_mt_rt,nullptr);
    pthread_create(&t5,nullptr,smalloc_test_mt_rt,nullptr);
    pthread_create(&t6,nullptr,smalloc_test_mt_rt,nullptr);

    pthread_join(t6,nullptr);
    pthread_join(t5,nullptr);
    pthread_join(t4,nullptr);
    pthread_join(t3,nullptr);
    pthread_join(t2,nullptr);
    pthread_join(t1,nullptr);
    printf("smalloc_test_multithread --- pass\n");
}

void smalloc_test_cpp_thread(){
    auto tfunc = [&]{
        void* ptr = smalloc(4);
        sfree(ptr);
    };
    std::thread t1(tfunc);
    std::thread t2(tfunc);
    t1.join();
    t2.join();
}


int main(){ 

    //smalloc_test_multithread();

    smalloc_test_speed(32,10000);

    //smalloc_test_cpp_thread();

    return 0;
}