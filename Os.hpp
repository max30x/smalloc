#pragma once

#include <unistd.h>
#include <sys/mman.h>
#include <cstddef>

#define CACHELINE 64
#define PAGE 4096
#define PAGESHIFT 12

#define pad long

#define PTRSIZE sizeof(void*)

#define CPUNUM \
    sysconf(_SC_NPROCESSORS_ONLN)

#define CPUNUM_S 4

void* os_to_page(void *addr,std::size_t length,bool exec);
int page_to_os(void *addr,std::size_t length);
int page_to_corner(void *addr,std::size_t length);

