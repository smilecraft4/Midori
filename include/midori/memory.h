#pragma once

#include <SDL3/SDL.h>

#include <cstdlib>
#include <cstring>

#if defined(NDEBUG) && defined(TRACY_ENABLE)
#undef TRACY_ENABLE
#endif
#include <tracy/Tracy.hpp>

namespace Midori {

void *Malloc(size_t size);
void *Calloc(size_t nmemb, size_t size);
void *Realloc(void *mem, size_t size);
void Free(void *mem);
};  // namespace Midori

void *operator new(std::size_t count);
void operator delete(void *ptr) noexcept;