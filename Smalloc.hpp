#pragma once

#include <cstdint>

extern "C" {
    void* smalloc(std::size_t size);
    void* scalloc(std::size_t nitems,std::size_t size);
    void sfree(void* ptr);
}
