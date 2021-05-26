#pragma once

#include <cstdint>

extern "C" {
    void* smalloc(std::size_t size);
    void sfree(void* ptr);
}
