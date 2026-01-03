#include "memory.h"

#include <cstdlib>
#include <cstring>

#include <SDL3/SDL.h>
#if defined(NDEBUG) && defined(TRACY_ENABLE)
#undef TRACY_ENABLE
#endif
#include <tracy/Tracy.hpp>

void *Midori::Malloc(size_t size) {
    void *mem = std::malloc(size);
    TracyAlloc(mem, size);
    return mem;
};

void *Midori::Calloc(size_t nmemb, size_t size) {
    void *mem = std::calloc(nmemb, size);
    TracyAlloc(mem, size);
    return mem;
};

void *Midori::Realloc(void *mem, size_t size) {
    TracyFree(mem);
    void *new_mem = std::realloc(mem, size);
    TracyAlloc(new_mem, size);
    return new_mem;
};

void Midori::Free(void *mem) {
    TracyFree(mem);
    std::free(mem);
}

void *operator new(std::size_t count) {
    if (count == 0) {
        count = 1;
    }
    auto *ptr = Midori::Malloc(count);
    return ptr;
}
void operator delete(void *ptr) noexcept { Midori::Free(ptr); }