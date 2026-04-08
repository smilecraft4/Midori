#include "memory.h"

#include <cstdlib>
#include <cstring>

#include <SDL3/SDL.h>
#include <tracy/Tracy.hpp>

// TODO: Use aligned alloc as much as possible

void* Midori::Malloc(size_t size) {
    void* mem = std::malloc(size);
    TracyAlloc(mem, size);
    return mem;
};

void* Midori::Calloc(size_t nmemb, size_t size) {
    void* mem = std::calloc(nmemb, size);
    TracyAlloc(mem, size);
    return mem;
};

void* Midori::Realloc(void* mem, size_t size) {
    TracyFree(mem);
    void* new_mem = std::realloc(mem, size);
    TracyAlloc(new_mem, size);
    return new_mem;
};

void Midori::Free(void* mem) {
    TracyFree(mem);
    std::free(mem);
}

void* operator new(std::size_t count) {
    if (count == 0) {
        count = 1;
    }
    auto* ptr = Midori::Malloc(count);
    return ptr;
}

void operator delete(void* ptr) noexcept {
    Midori::Free(ptr);
}
void* operator new[](std::size_t count, const char* name, int flags,
                     unsigned int debug_flags, const char* file, int line)
{
    (void)name; (void)flags; (void)debug_flags; (void)file; (void)line;
    if (count == 0) count = 1;
    return Midori::Malloc(count);
}

// EASTL operator new[](size, alignment, offset, name, flags, debug_flags, file, line)
void* operator new[](std::size_t count, std::size_t alignment, std::size_t offset,
                     const char* name, int flags,
                     unsigned int debug_flags, const char* file, int line)
{
    (void)name; (void)flags; (void)debug_flags; (void)file; (void)line;
    if (count == 0) count = 1;
    return Midori::Malloc(count); // use offset if your allocator supports it
}

void operator delete[](void* ptr) noexcept {
    Midori::Free(ptr);
}

// Aligned variants (needed if you use EA_ALIGNED / EASTLAlignedNew)
void* operator new(std::size_t count, std::size_t alignment, std::size_t offset,
                   const char* name, int flags,
                   unsigned int debug_flags, const char* file, int line)
{
    (void)name; (void)flags; (void)debug_flags; (void)file; (void)line;
    if (count == 0) count = 1;
    return Midori::Malloc(count);
}