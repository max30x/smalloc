#pragma once

#include <sys/time.h>
#include <stdio.h>

#define RATIO 1000000

namespace utils{
class ScopeTimer{
public:
    ScopeTimer(){
        gettimeofday(&start,nullptr);
    }

    ~ScopeTimer(){
        gettimeofday(&end,nullptr);
        long a = start.tv_sec*RATIO + start.tv_usec;
        long b = end.tv_sec*RATIO + end.tv_usec;
        long diff = b - a;
        printf("sec:%ld  usec:%ld\n",diff/RATIO,diff%RATIO);
    }

private:
    struct timeval start;
    struct timeval end;
};
}